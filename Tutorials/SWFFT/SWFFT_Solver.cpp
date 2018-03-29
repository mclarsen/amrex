
#include <SWFFT_Solver.H>
#include <SWFFT_Solver_F.H>

#include <AMReX_MultiFabUtil.H>
#include <AMReX_VisMF.H>
#include <AMReX_ParmParse.H>

// These are for SWFFT
#include <Distribution.H>
#include <AlignedAllocator.h>
#include <Dfft.H>

#include <string>

#define ALIGN 16

using namespace amrex;

SWFFT_Solver::SWFFT_Solver ()
{
    static_assert(AMREX_SPACEDIM == 3, "3D only");

    // runtime parameters
    {
        ParmParse pp;

        // Read in n_cell.  Use defaults if not explicitly defined.
        int cnt = pp.countval("n_cell");
        std::cout << "cnt " << std::endl;
        if (cnt > 1) {
            Vector<int> ncs;
            pp.getarr("n_cell",ncs);
            n_cell = IntVect{ncs[0],ncs[1],ncs[2]};
        } else if (cnt > 0) {
            int ncs;
            pp.get("n_cell",ncs);
            n_cell = IntVect{ncs,ncs,ncs};
        } else {
           n_cell = IntVect{32,32,32};
        }
        std::cout << "N_CELL " << n_cell << std::endl;

        // Read in max_grid_size.  Use defaults if not explicitly defined.
        cnt = pp.countval("max_grid_size");
        if (cnt > 1) {
            Vector<int> mgs;
            pp.getarr("max_grid_size",mgs);
            max_grid_size = IntVect{mgs[0],mgs[1],mgs[2]};
        } else if (cnt > 0) {
            int mgs;
            pp.get("max_grid_size",mgs);
            max_grid_size = IntVect{mgs,mgs,mgs};
        } else {
           max_grid_size = IntVect{32,32,32};
        }

        pp.query("verbose", verbose);
    }
    
    BoxArray ba;
    {
        IntVect dom_lo(0,0,0);
        IntVect dom_hi(n_cell[0]-1,n_cell[1]-1,n_cell[2]-1);
        Box domain(dom_lo,dom_hi);
        ba.define(domain);
        ba.maxSize(max_grid_size);

        // We assume a unit box (just for convenience)
        RealBox real_box({0.0,0.0,0.0}, {1.0,1.0,1.0});

        // The FFT assumes fully periodic boundaries
        std::array<int,3> is_periodic {1,1,1};

        geom.define(domain, &real_box, CoordSys::cartesian, is_periodic.data());
    }

    // Make sure we define both the soln and the rhs with the same DistributionMapping 
    DistributionMapping dmap{ba};

    // Note that we are defining rhs with NO ghost cells
    rhs.define(ba, dmap, 1, 0);
    init_rhs();

    // Note that we are defining soln with NO ghost cells
    soln.define(ba, dmap, 1, 0);
    the_soln.define(ba, dmap, 1, 0);

    comp_the_solution();
}

void
SWFFT_Solver::init_rhs ()
{
    const Real* dx = geom.CellSize();

    for (MFIter mfi(rhs,true); mfi.isValid(); ++mfi)
    {
        const Box& tbx = mfi.tilebox();
        fort_init_rhs(BL_TO_FORTRAN_BOX(tbx),
                      BL_TO_FORTRAN_ANYD(rhs[mfi]),
                      dx);
    }

    Real sum_rhs = rhs.sum();
    amrex::Print() << "Sum of rhs over the domain is " << sum_rhs << std::endl;
}

void
SWFFT_Solver::comp_the_solution ()
{
    const Real* dx = geom.CellSize();

    for (MFIter mfi(the_soln); mfi.isValid(); ++mfi)
    {
        fort_comp_asol(BL_TO_FORTRAN_ANYD(the_soln[mfi]),dx);
    }
}

void
SWFFT_Solver::solve ()
{
    const BoxArray& ba = soln.boxArray();
    std::cout << "BA " << ba << std::endl;
    exit(0);
    const DistributionMapping& dm = soln.DistributionMap();

    // If true the write out the multifabs for rhs, soln and exact_soln
    bool write_data = false;

    // Define pi and (two pi) here
    Real  pi = 4 * std::atan(1.0);
    Real tpi = 2 * pi;

    // We assume that all grids have the same size hence 
    // we have the same nx,ny,nz on all ranks
    int nx = ba[0].size()[0];
    int ny = ba[0].size()[1];
    int nz = ba[0].size()[2];

    Box domain(geom.Domain());

    int nbx = domain.length(0) / nx;
    int nby = domain.length(1) / ny;
    int nbz = domain.length(2) / nz;
    int nboxes = nbx * nby * nbz;
    if (nboxes != ba.size()) 
       amrex::Error("NBOXES NOT COMPUTED CORRECTLY");

    Vector<int> rank_mapping;
    rank_mapping.resize(nboxes);

    DistributionMapping dmap = rhs.DistributionMap();

    if (verbose)
       amrex::Print() << "NBX NBY NBZ " << nbx << " " << nby << " "<< nbz  << std::endl;
    for (int ib = 0; ib < nboxes; ++ib)
    {
        int i = ba[ib].smallEnd(0) / nx;
        int j = ba[ib].smallEnd(1) / ny;
        int k = ba[ib].smallEnd(2) / nz;

        // This would be the "correct" local index if the data was
        // int local_index = k*nbx*nby + j*nbx + i;

        // This is what we pass to dfft to compensate for the Fortran ordering
        //      of amrex data in MultiFabs.
        int local_index = k*nbx*nby + j*nbx + i;

        rank_mapping[local_index] = dmap[ib];
        if (verbose)
          amrex::Print() << "LOADING RANK NUMBER " << dmap[ib] << " FOR GRID NUMBER " << ib 
                         << " WHICH IS LOCAL NUMBER " << local_index << std::endl;
    }

    if (verbose)
      for (int ib = 0; ib < nboxes; ++ib)
        amrex::Print() << "GRID IB " << ib << " IS ON RANK " << rank_mapping[ib] << std::endl;

    int n = domain.length(0);
    Real hsq = 1. / (n*n);

    Real start_time = amrex::second();

    // Assume for now that nx = ny = nz
    int Ndims[3] = { nbx, nby, nbz };
    hacc::Distribution d(MPI_COMM_WORLD,n,Ndims,&rank_mapping[0]);
    hacc::Dfft dfft(d);
    
    for (MFIter mfi(rhs,false); mfi.isValid(); ++mfi)
    {
       int gid = mfi.index();

       size_t local_size  = dfft.local_size();
   
       std::vector<complex_t, hacc::AlignedAllocator<complex_t, ALIGN> > a;
       std::vector<complex_t, hacc::AlignedAllocator<complex_t, ALIGN> > b;

       a.resize(nx*ny*nz);
       b.resize(nx*ny*nz);

       dfft.makePlans(&a[0],&b[0],&a[0],&b[0]);

       // Copy real data from Rhs into real part of a -- no ghost cells and
       // put into C++ ordering (not Fortran)
       complex_t zero(0.0, 0.0);
       size_t local_indx = 0;
       for(size_t i=0; i<(size_t)nx; i++) {
        for(size_t j=0; j<(size_t)ny; j++) {
         for(size_t k=0; k<(size_t)nz; k++) {

           complex_t temp(rhs[mfi].dataPtr()[local_indx],0.);
           a[local_indx] = temp;
      	   local_indx++;

         }
       }
      }

//  *******************************************
//  Compute the forward transform
//  *******************************************
    dfft.forward(&a[0]);

//  *******************************************
//  Now divide the coefficients of the transform
//  *******************************************
    local_indx = 0;
    const int *self = dfft.self_kspace();
    const int *local_ng = dfft.local_ng_kspace();
    const int *global_ng = dfft.global_ng();
    for(size_t i=0; i<(size_t)local_ng[0]; i++) {
     size_t global_i = local_ng[0]*self[0] + i; 
     for(size_t j=0; j<(size_t)local_ng[1]; j++) { 
      size_t global_j = local_ng[1]*self[1] + j;
      for(size_t k=0; k<(size_t)local_ng[2]; k++) {
        size_t global_k = local_ng[2]*self[2] + k;

        if (global_i == 0 && global_j == 0 & global_k == 0) {
           a[local_indx] = 0;
        } else {
           double fac = 2. * (
                        (cos(tpi*double(global_i)/double(global_ng[0])) - 1.) +
                        (cos(tpi*double(global_j)/double(global_ng[1])) - 1.) +
                        (cos(tpi*double(global_k)/double(global_ng[2])) - 1.) );

           a[local_indx] = a[local_indx] / fac;
        }
	local_indx++;

      }
     }
    }

//     *******************************************
//     Compute the backward transform
//     *******************************************
       dfft.backward(&a[0]);

       size_t global_size  = dfft.global_size();
       double fac = hsq / global_size;

       local_indx = 0;
       for(size_t i=0; i<(size_t)nx; i++) {
        for(size_t j=0; j<(size_t)ny; j++) {
         for(size_t k=0; k<(size_t)nz; k++) {

           // Divide by 2 pi N
           soln[mfi].dataPtr()[local_indx] = fac * std::real(a[local_indx]);
      	   local_indx++;

         }
        }
       }
    }
    Real total_time = amrex::second() - start_time;

    if (write_data)
    {
       VisMF::Write(rhs,"RHS");
       VisMF::Write(soln,"SOL_COMP");
       VisMF::Write(the_soln,"SOL_EXACT");
    }

    if (verbose)
    {
       amrex::Print() << "MAX / MIN VALUE OF COMP  SOLN " <<     soln.max(0) << " " 
                      <<     soln.min(0) << std::endl;
       amrex::Print() << "MAX / MIN VALUE OF EXACT SOLN " << the_soln.max(0) << " " 
                      << the_soln.min(0) << std::endl;
    }

    MultiFab diff(ba, dm, 1, 0);
    MultiFab::Copy(diff, soln, 0, 0, 1, 0);
    MultiFab::Subtract(diff, the_soln, 0, 0, 1, 0);
    amrex::Print() << "\nMax-norm of the error is " << diff.norm0() << "\n";
    amrex::Print() << "Time spent in solve: " << total_time << std::endl;

    if (write_data)
       VisMF::Write(diff,"DIFF");
}
AMREX_HOME ?= ../../../

DEBUG = FALSE
DIM = 3
COMP = gnu

USE_MPI = TRUE
USE_OMP = FALSE
USE_CUDA = FALSE 
TINY_PROFILE = TRUE

MPI_MULTIPLE = TRUE


include $(AMREX_HOME)/Tools/GNUMake/Make.defs

include ./Make.package
include $(AMREX_HOME)/Src/Base/Make.package

include $(AMREX_HOME)/Tools/GNUMake/Make.rules

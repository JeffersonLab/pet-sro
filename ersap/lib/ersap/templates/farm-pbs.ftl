#!/bin/csh

#
# Copyright (c) 2021.  Jefferson Science Associates, LLC.
# Subject to the terms in the LICENSE file found in the top-level directory.
# Author gyurjyan
#

#PBS -N rec-${user}-${description}
#PBS -A clas12
#PBS -S /bin/csh
#PBS -l nodes=1:ppn=${farm.cpu}
#PBS -l file=${farm.disk}kb
#PBS -l walltime=${farm.time}

"${farm.script}"

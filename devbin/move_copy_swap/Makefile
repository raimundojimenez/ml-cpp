#
# Copyright Elasticsearch B.V. and/or licensed to Elasticsearch B.V. under one
# or more contributor license agreements. Licensed under the Elastic License;
# you may not use this file except in compliance with the Elastic License.
#
include $(CPP_SRC_HOME)/mk/defines.mk

TARGET=move_copy_swap$(EXE_EXT)

ML_LIBS=$(LIB_ML_CORE)

LIBS=$(ML_LIBS)

all: build

SRCS= \
    Main.cc \

NO_TEST_CASES=1

include $(CPP_SRC_HOME)/mk/stddevapp.mk


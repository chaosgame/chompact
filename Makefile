CFLAGS=-std=c++0x -g
CXX=g++

chompact: chompact2.cpp
	$(CXX) $(CFLAGS) $? -o $@

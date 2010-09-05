CFLAGS=-std=c++0x -g
CXX=g++

chompact: chompact.cpp
	$(CXX) $(CFLAGS) $? -o $@

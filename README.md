# cpp2c

A sample program to generate C bindings for public functions in a C++ header.

## Dependencies
- Clang
- LLVM

Install clang and llvm from [here](https://releases.llvm.org/download.html).

## How to build
```console
mkdir build
cd build
cmake -DCMAKE_INSTALL_PREFIX=/home/karthik/cpp2c/install ..
make
make install
```

## How to run
```console
cd install/bin
./cpp2c /home/karthik/OpenTimelineIO/src/opentime/rationalTime.h -- -I/home/karthik/OpenTimelineIO/src/
```

This program uses clang AST matcher to find public methods to generate C wrapper. We also need to specify the include directories for the C++ standard library headers. These need to be passed in as command line arguments. Currently these arguments (specific to my machine) have been appended to the `**argv` variable in code. 

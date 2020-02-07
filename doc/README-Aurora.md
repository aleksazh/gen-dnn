# SX-Aurora porting

### Prerequisites:

Aurora C and C++ compiler ncc and nc++ should be in $PATH. That means usually:
```
export PATH=/opt/nec/ve/bin:$PATH
```

The environment variable NLC_BASE should point to the location of the NEC Numeric Library Collection, for example:

```
export NLC_BASE=/opt/nec/ve/nlc/0.9.0
```

The ```-proginf``` build option links with libveperf, which must be added to the linker path:

```
export LDFLAGS="-L/usr/uhome/aurora/mpc/pub/veperf/latest/lib"
or
export LDFLAGS="-L/opt/nec/ve/veperf/latest/lib"
```

### Building

```
./build.sh -h # help
CC=ncc CXX=nc++ ./build.sh -a
```

*cmake* invoked with the ve.cmake TOOLCHAIN (as in *build.sh -a*) will set up
*ncc* directories by looking in standard locations, even if not in your PATH.

#### dnnl v1.1 port

- nc++ still has sporadic (reproducible) initialization errors
  - it sometimes does not 'zero-initialize' POD structs in C++ code.
- nc++ has trouble with reorders (too many functions?)

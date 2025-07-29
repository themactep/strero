Thingino Streamer is a streaming server app for Ingenic-based IP cameras
------------------------------------------------------------------------

### Code

The code is located in `src/` directory.

We build for 32-bit MIPS platform, so using 64-bit types is not safe.
Please replace all uint64_t with uint32_t.

We do not use walltime clock as a timestamp source.
Please use `get_monotonic_timestamp` utility function from `common.h`.

### Modular Architecture

Thingino uses modular architecture to enable/disable features at compile time.
Each module is self-contained in `src/modules/` directory.

To create a new module, follow the steps in `doc/MODULAR_SYSTEM.md`.

### Building

The project is not self-contained and cannot be built standalone.
Compilation is done via Thingino buildroot.
Use `./build.sh` script to build and copy the compiled binary to the NFS share.

### Running

Mount the share on the camera.
```bash
mount -o nolock 192.168.1.61:/hoem/paul/nfs /mnt/nfs
```

Then run every newly compiled binary directly from the mount
```bash
/mnt/nfs/streamer
```

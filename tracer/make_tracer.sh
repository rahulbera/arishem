export PIN_ROOT=/home/rahbera/softwares/pin-external-3.31-98869-gfa6f126a8-gcc-linux
export ZSTD_HOME=/home/rahbera/local
mkdir -p obj-intel64
#make obj-intel64/champsim_tracer.so
make obj-intel64/champsim_tracer_mt_roi.so
make obj-intel64/champsim_tracer_mt_roi_v2.so
make obj-intel64/champsim_tracer_mt_roi_v3.so

#!/bin/sh
# Fetches the Spiking Heidelberg Digits (Cramer et al., 2020; CC BY 4.0)
# into build/snn_train/data, which is not part of the repository: spoken
# digits 0 to 9 in English and German through a model of the cochlea,
# 700 channels of spikes, 8156 samples to train on and 2264 to test, as
# HDF5 (about 130 and 38 MB packed). Checked against the md5 sums its
# authors give. Then examples/snn_train/prepare.py makes them flat.
set -e
cd "$(dirname "$0")/../.."
mkdir -p build/snn_train/data
cd build/snn_train/data
for name in shd_train shd_test; do
  if [ ! -f "$name.h5" ]; then
    curl -fL --retry 3 -o "$name.h5.gz" "https://zenkelab.org/datasets/$name.h5.gz"
    case "$name" in
      shd_train) sum=d47c9825dee33347913e8ce0f2be08b0 ;;
      shd_test) sum=3062a80ec0c5719404d5b02e166543b1 ;;
    esac
    echo "$sum  $name.h5.gz" | md5sum -c -
    gunzip -f "$name.h5.gz"
  fi
done
echo "SHD in build/snn_train/data; examples/snn_train/prepare.py makes it ready"

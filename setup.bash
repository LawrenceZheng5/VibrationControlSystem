#!/usr/bin/env bash

# For setup and downloading necessary packages 
sudo apt install -y alsa-utils
sudo apt install -y cmake
sudo apt install -y pkg-config 
sudo apt install -y portaudio19-dev 
sudo apt install -y libcfitsio-dev 

# Milk specfic dependencies
sudo apt install -y libgsl-dev 
sudo apt install -y libfftw3-dev 
sudo apt install -y libncurses-dev 
sudo apt install -y libreadline-dev 
sudo apt install -y bison flex 
sudo apt install -y libhwloc-dev 
sudo apt install -y libopenblas-dev 
sudo apt install -y liblapacke-dev 
sudo apt-get install -y \
    git \
    make \
    dpkg-dev \
    libc6-dev \
    cmake \
    pkg-config \
    python3-dev \
    libcfitsio-dev \
    pybind11-dev \
    python3-pybind11 \
    libgsl-dev \
    libfftw3-dev \
    libncurses-dev \
    libbison-dev \
    libfl-dev \
    libreadline-dev \
    gfortran libopenblas-dev liblapacke-dev \
    pkg-config \
    gcc \
    g++ \
    pip \
    python3-pip \
    nnn

sudo apt install -y cpuset 
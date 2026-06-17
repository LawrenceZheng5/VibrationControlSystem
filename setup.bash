#!/usr/bin/env bash

# For setup and downloading necessary packages 
sudo apt install alsa-utils
sudo apt install cmake
sudo apt install pkg-config 
sudo apt install portaudio19-dev 
sudo apt install libcfitsio-dev 

# Milk specfic dependencies
sudo apt install libgsl-dev 
sudo apt install libfftw3-dev 
sudo apt install libncurses-dev 
sudo apt install libreadline-dev 
sudo apt install bison flex 
sudo apt install libhwloc-dev 
sudo apt install libopenblas-dev 
sudo apt install liblapacke-dev 
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
    g++

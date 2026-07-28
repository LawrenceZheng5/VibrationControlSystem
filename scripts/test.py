#!/usr/bin/env python3

from pathlib import Path
import argparse

import numpy as np
import matplotlib.pyplot as plt
from matplotlib.colors import LogNorm
from astropy.io import fits

def main():
    parser = argparse.ArgumentParser(description="Test script for Vibration Control System")
    parser.add_argument("directory", type=Path, help="Path to FITS file")
    args = parser.parse_args()

    for file in args.directory.iterdir():
        if file.suffix == ".fits":
            with fits.open(file) as hdul:
                hdul.info()

                for hdu in hdul:
                    print(hdu.header)
                    # print(hdu.header["DATE"])
                    if hdu.data is not None:
                        print(hdu.data.shape)
                        print(hdu.data)
                


if __name__ == "__main__":
    main()

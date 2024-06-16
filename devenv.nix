{ pkgs, ... }:

{
  packages = with pkgs; [
    bzip2
    cairo
    cfitsio
    libjpeg
    python3Packages.setuptools
    swig
    zlib
  ];

  languages.c.enable = true;
  languages.python.enable = true;
}

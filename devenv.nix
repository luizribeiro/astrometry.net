{ pkgs, ... }:

{
  packages = with pkgs; [
    bzip2
    cairo
    cfitsio
    libjpeg
    zlib
  ];

  languages.c.enable = true;
}

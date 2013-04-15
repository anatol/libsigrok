##
## This file is part of the sigrok project.
##
## Copyright (C) 2013 Martin Ling <martin-sigrok@earth.li>
##
## This program is free software: you can redistribute it and/or modify
## it under the terms of the GNU General Public License as published by
## the Free Software Foundation, either version 3 of the License, or
## (at your option) any later version.
##
## This program is distributed in the hope that it will be useful,
## but WITHOUT ANY WARRANTY; without even the implied warranty of
## MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
## GNU General Public License for more details.
##
## You should have received a copy of the GNU General Public License
## along with this program.  If not, see <http://www.gnu.org/licenses/>.
##

from distutils.core import setup, Extension
import subprocess

sr_includes = subprocess.check_output(
    ["pkg-config", "--cflags", "libsigrok"]).rstrip().split(' ')

sr_libs = subprocess.check_output(
    ["pkg-config", "--libs", "libsigrok"]).rstrip().split(' ')

sr_version = subprocess.check_output(
    ["pkg-config", "--version", "libsigrok"]).rstrip()

setup(
    name = 'libsigrok',
    version = sr_version,
    description = "libsigrok API wrapper",
    py_modules = ['libsigrok'],
    ext_modules = [
        Extension('_libsigrok',
            sources = ['libsigrok_python.i'],
            swig_opts = sr_includes,
            include_dirs = [i[2:] for i in sr_includes if i.startswith('-I')],
            library_dirs = [l[2:] for l in sr_libs if l.startswith('-L')],
            libraries = [l[2:] for l in sr_libs if l.startswith('-l')]
        )
    ],
)
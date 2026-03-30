#!/usr/bin/env python3

import sys
import traceback


# quick version check
if sys.version_info < (3, 11):
    print("Sorry, the Overviewer requires at least Python 3.11 to run.")
    sys.exit(1)


from setuptools import setup, Extension
from setuptools.command.build import build
from setuptools.command.build_ext import build_ext
from setuptools.command.sdist import sdist
import logging
import os
import os.path
import glob
import platform
import sysconfig
import time
import overviewer_core.util as util
import numpy


# make sure our current working directory is the same directory
# setup.py is in
curdir = os.path.split(sys.argv[0])[0]
if curdir:
    os.chdir(curdir)

# keyword arguments for setup
setup_kwargs = {}
setup_kwargs['ext_modules'] = []
setup_kwargs['cmdclass'] = {}
setup_kwargs['options'] = {}

#
# metadata
#

# Utility function to read the README file.
# Used for the long_description.  It's nice, because now 1) we have a top level
# README file and 2) it's easier to type in the README file than to put a raw
# string in below ...
def read(fname):
    return open(fname).read()

setup_kwargs['name'] = 'Minecraft-Overviewer'
setup_kwargs['version'] = util.findGitTag()
setup_kwargs['description'] = 'Generates large resolution images of a Minecraft map.'
setup_kwargs['url'] = 'http://overviewer.org/'
setup_kwargs['author'] = 'Andrew Brown'
setup_kwargs['author_email'] = 'brownan@gmail.com'
setup_kwargs['license'] = 'GNU General Public License v3'
setup_kwargs['long_description'] = read('README.rst')

# top-level files that should be included as documentation
doc_files = ['COPYING.txt', 'README.rst', 'CONTRIBUTORS.rst', 'sample_config.py']

# helper to create a 'package_data'-type sequence recursively for a given dir
def recursive_package_data(src, package_dir='overviewer_core'):
    full_src = os.path.join(package_dir, src)
    ret = []
    for dirpath, dirnames, filenames in os.walk(full_src, topdown=False):
        current_path = os.path.relpath(dirpath, package_dir)
        for filename in filenames:
            ret.append(os.path.join(current_path, filename))

    return ret

# Finds the system-wide path from within a venv.
# Taken from https://github.com/pyinstaller/pyinstaller/blob/master/PyInstaller/hooks/pre_find_module_path/hook-distutils.py
def find_system_module_path():
    # opcode is not a virtualenv module, so we can use it to find the stdlib. Technique taken from virtualenv's
    # "distutils" package detection at
    # https://github.com/pypa/virtualenv/blob/16.3.0/virtualenv_embedded/distutils-init.py#L5
    import opcode

    system_module_path = os.path.normpath(os.path.dirname(opcode.__file__))
    return system_module_path

#
# script, package, and data
#

setup_kwargs['packages'] = ['overviewer_core', 'overviewer_core/aux_files']
setup_kwargs['scripts'] = ['overviewer.py']
setup_kwargs['package_data'] = {'overviewer_core': recursive_package_data('data/textures') + recursive_package_data('data/web_assets') + recursive_package_data('data/js_src')}

setup_kwargs['data_files'] = [('share/doc/minecraft-overviewer', doc_files)]


#
# c_overviewer extension
#

# Obtain the numpy include directory
numpy_include = numpy.get_include()

try:
    pil_include = os.environ['PIL_INCLUDE_DIR'].split(os.pathsep)
except Exception:
    pil_include = [os.path.join(sysconfig.get_path('platinclude'), 'Imaging')]
    if not os.path.exists(pil_include[0]):
        pil_include = []


# used to figure out what files to compile
# auto-created from files in primitives/, but we need the raw names so
# we can use them later.
primitives = []
for name in glob.glob("overviewer_core/src/primitives/*.c"):
    name = os.path.split(name)[-1]
    name = os.path.splitext(name)[0]
    primitives.append(name)

c_overviewer_files = ['main.c', 'composite.c', 'iterate.c', 'endian.c', 'rendermodes.c', 'block_class.c']
c_overviewer_files += ['primitives/%s.c' % (mode) for mode in primitives]
c_overviewer_files += ['Draw.c']
c_overviewer_includes = ['overviewer.h', 'rendermodes.h']

c_overviewer_files = ['overviewer_core/src/' + s for s in c_overviewer_files]
c_overviewer_includes = ['overviewer_core/src/' + s for s in c_overviewer_includes]

# Workaround for virtualenv overriding base_prefix on Windows,
# which can prevent finding the Python library directory
python_lib_dirs = None
if platform.system() == 'Windows':
    ci_python_dir = os.path.split(find_system_module_path())[0]
    python_lib_dirs = [os.path.join(ci_python_dir, "Libs")]

setup_kwargs['ext_modules'].append(Extension(
    'overviewer_core.c_overviewer',
    c_overviewer_files,
    include_dirs=['.', numpy_include] + pil_include,
    library_dirs=python_lib_dirs,
    depends=c_overviewer_includes,
    extra_link_args=[]
))


# tell build_ext to build the extension in-place
# (NOT in build/)
setup_kwargs['options']['build_ext'] = {'inplace' : 1}

log = logging.getLogger(__name__)

# custom clean command to remove in-place extension and primitives header
class CustomClean(build):
    description = "clean build artifacts"

    def run(self):
        import shutil
        # Remove build directories
        for d in ['build', 'dist', '*.egg-info']:
            for path in glob.glob(d):
                if os.path.isdir(path):
                    log.info("removing '%s'", path)
                    shutil.rmtree(path)

        # try to remove c_overviewer extension
        build_ext_cmd = self.get_finalized_command('build_ext')
        ext_fname = build_ext_cmd.get_ext_filename('overviewer_core.c_overviewer')
        primspath = os.path.join("overviewer_core", "src", "primitives.h")

        for fname in [ext_fname, primspath]:
            if os.path.exists(fname):
                try:
                    log.info("removing '%s'", fname)
                    os.remove(fname)
                except OSError:
                    log.warning("'%s' could not be cleaned -- permission denied", fname)

        # purge all *.pyc files
        for root, dirs, files in os.walk(os.path.join(os.path.dirname(__file__), ".")):
            for f in files:
                if f.endswith(".pyc"):
                    os.remove(os.path.join(root, f))

def generate_version_py():
    try:
        outstr = ""
        outstr += "VERSION=%r\n" % util.findGitTag()
        outstr += "HASH=%r\n" % util.findGitHash()
        outstr += "BUILD_DATE=%r\n" % time.asctime()
        outstr += "BUILD_PLATFORM=%r\n" % platform.processor()
        outstr += "BUILD_OS=%r\n" % platform.platform()
        f = open("overviewer_core/overviewer_version.py", "w")
        f.write(outstr)
        f.close()
    except Exception:
        print("WARNING: failed to build overviewer_version file")

def generate_primitives_h():
    global primitives
    prims = [p.lower().replace('-', '_') for p in primitives]

    outstr = "/* this file is auto-generated by setup.py */\n"
    for p in prims:
        outstr += "extern RenderPrimitiveInterface primitive_{0};\n".format(p)
    outstr += "static RenderPrimitiveInterface *render_primitives[] = {\n"
    for p in prims:
        outstr += "    &primitive_{0},\n".format(p)
    outstr += "    NULL\n"
    outstr += "};\n"

    with open("overviewer_core/src/primitives.h", "w") as f:
        f.write(outstr)

class CustomSDist(sdist):
    def run(self):
        # generate the version file
        generate_version_py()
        generate_primitives_h()
        sdist.run(self)

class CustomBuild(build):
    def run(self):
        # generate the version file
        try:
            generate_version_py()
            generate_primitives_h()
            build.run(self)
            print("\nBuild Complete")
        except Exception:
            traceback.print_exc(limit=1)
            print("\nFailed to build Overviewer!")
            print("Please review the errors printed above and the build instructions")
            print("at <http://docs.overviewer.org/en/latest/building/>.  If you are")
            print("still having build problems, file an incident on the github tracker")
            print("or find us in IRC.")
            sys.exit(1)

class CustomBuildExt(build_ext):
    def build_extensions(self):
        c = self.compiler.compiler_type
        if c == "msvc":
            # customize the build options for this compilier
            for e in self.extensions:
                e.extra_link_args.append("/MANIFEST")
                e.extra_link_args.append("/DWINVER=0x060")
                e.extra_link_args.append("/D_WIN32_WINNT=0x060")
        if c == "unix":
            # customize the build options for this compilier
            for e in self.extensions:
                e.extra_compile_args.append("-Wno-unused-variable") # quell some annoying warnings
                e.extra_compile_args.append("-Wno-unused-function") # quell some annoying warnings
                e.extra_compile_args.append("-Wdeclaration-after-statement")
                e.extra_compile_args.append("-Werror=declaration-after-statement")
                e.extra_compile_args.append("-O3")
                e.extra_compile_args.append("-std=gnu99")


        # build in place, and in the build/ tree
        self.inplace = False
        build_ext.build_extensions(self)
        self.inplace = True
        build_ext.build_extensions(self)


setup_kwargs['cmdclass']['clean'] = CustomClean
setup_kwargs['cmdclass']['sdist'] = CustomSDist
setup_kwargs['cmdclass']['build'] = CustomBuild
setup_kwargs['cmdclass']['build_ext'] = CustomBuildExt
###

setup(**setup_kwargs)


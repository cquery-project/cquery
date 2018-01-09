#!/usr/bin/env python
# encoding: utf-8

try:
    from urllib2 import urlopen  # Python 2
except ImportError:
    from urllib.request import urlopen  # Python 3

import os.path
import string
import subprocess
import sys
import re
import ctypes
import shutil

VERSION = '0.0.1'
APPNAME = 'cquery'

top = '.'
out = 'build'

CLANG_TARBALL_NAME = None
CLANG_TARBALL_EXT = '.tar.xz'
if sys.platform == 'darwin':
  CLANG_TARBALL_NAME = 'clang+llvm-$version-x86_64-apple-darwin'
elif sys.platform.startswith('freebsd'):
  CLANG_TARBALL_NAME = 'clang+llvm-$version-amd64-unknown-freebsd10'
# It is either 'linux2' or 'linux3' before Python 3.3
elif sys.platform.startswith('linux'):
  # These executable depend on libtinfo.so.5
  CLANG_TARBALL_NAME = 'clang+llvm-$version-x86_64-linux-gnu-ubuntu-14.04'
elif sys.platform == 'win32':
  CLANG_TARBALL_NAME = 'LLVM-$version-win64'
  CLANG_TARBALL_EXT = '.exe'

from waflib.Tools.compiler_cxx import cxx_compiler
cxx_compiler['linux'] = ['clang++', 'g++']

if sys.version_info < (3, 0):
  if sys.platform == 'win32':
    kdll = ctypes.windll.kernel32
    def symlink(source, link_name, target_is_directory=False):
      # SYMBOLIC_LINK_FLAG_DIRECTORY: 0x1
      SYMBOLIC_LINK_FLAG_ALLOW_UNPRIVILEGED_CREATE = 0x2
      flags = int(target_is_directory)
      ret = kdll.CreateSymbolicLinkA(link_name, source, flags)
      if ret == 0:
        err = ctypes.WinError()
        ERROR_PRIVILEGE_NOT_HELD = 1314
        # Creating symbolic link on Windows requires a special priviledge SeCreateSymboliclinkPrivilege,
        # which an non-elevated process lacks. Starting with Windows 10 build 14972, this got relaxed
        # when Developer Mode is enabled. Triggering this new behaviour requires a new flag. Try again.
        if err[0] == ERROR_PRIVILEGE_NOT_HELD:
          flags |= SYMBOLIC_LINK_FLAG_ALLOW_UNPRIVILEGED_CREATE
          ret = kdll.CreateSymbolicLinkA(link_name, source, flags)
          if ret != 0:
            return
          err = ctypes.WinError()
        raise err
  else:
    # Python 3 compatibility
    real_symlink = os.symlink
    def symlink(source, link_name, target_is_directory=False):
      return real_symlink(source, link_name)
  os.symlink = symlink

# There is a null pointer dereference issue in tools/libclang/CXIndexDataConsumer.cpp handleReference.
def patch_byte_in_libclang(filename, offset, old, new):
  with open(filename, 'rb+') as f:
    f.seek(offset)
    t = f.read(1)
    if t == old:
      f.seek(offset)
      f.write(new)
      print('Applied byte patch hack at 0x{:x}'.format(offset))
      print('This is a makeshift for indexing default arguments of template template parameters, which otherwise would crash libclang')
      print('See https://github.com/jacobdufault/cquery/issues/219 for details')
    else:
      assert t == new

def options(opt):
  opt.load('compiler_cxx')
  grp = opt.add_option_group('Configuration options related to use of clang from the system (not recommended)')
  grp.add_option('--use-system-clang', dest='use_system_clang', default=False, action='store_true',
                 help='enable use of clang from the system')
  grp.add_option('--use-clang-cxx', dest='use_clang_cxx', default=False, action='store_true',
                 help='use clang C++ API')
  grp.add_option('--bundled-clang', dest='bundled_clang', default='5.0.1',
                 help='bundled clang version, downloaded from https://releases.llvm.org/ , e.g. 4.0.0 5.0.1')
  grp.add_option('--llvm-config', dest='llvm_config', default='llvm-config',
                 help='specify path to llvm-config for automatic configuration [default: %default]')
  grp.add_option('--clang-prefix', dest='clang_prefix', default='',
                 help='enable fallback configuration method by specifying a clang installation prefix (e.g. /opt/llvm)')
  grp.add_option('--variant', default='release',
                 help='variant name for saving configuration and build results. Variants other than "debug" turn on -O3')

def download_and_extract(destdir, url, ext):
  dest = destdir + ext

  # Extract the tarball.
  if not os.path.isdir(os.path.join(destdir, 'include')):
    # Download and save the compressed tarball as |compressed_file_name|.
    if not os.path.isfile(dest):
      print('Downloading tarball')
      print('   destination: {0}'.format(dest))
      print('   source:      {0}'.format(url))
      # TODO: verify checksum
      response = urlopen(url, timeout=60)
      with open(dest, 'wb') as f:
        f.write(response.read())
    else:
      print('Found tarball at {0}'.format(dest))

    print('Extracting {0}'.format(dest))
    # TODO: make portable.
    if ext == '.exe':
      subprocess.call(['7z', 'x', '-o{0}'.format(destdir), '-xr!$PLUGINSDIR', dest])
    else:
      subprocess.call(['tar', '-x', '-C', out, '-f', dest])
      # TODO Remove after migrating to a clang release newer than 5.0.1
      # For 5.0.1 Mac OS X, the directory and the tarball have different name
      if destdir == 'build/clang+llvm-5.0.1-x86_64-apple-darwin':
        os.rename(destdir.replace('5.0.1', '5.0.1-final'), destdir)
      # TODO Byte patch hack for other prebuilt binaries
      elif destdir == 'build/clang+llvm-4.0.0-x86_64-linux-gnu-ubuntu-14.04':
        patch_byte_in_libclang(os.path.join(destdir, 'lib/libclang.so.4.0'),
            0x4172b5, b'\x48', b'\x4d')
      elif destdir == 'build/clang+llvm-5.0.1-x86_64-linux-gnu-ubuntu-14.04':
        patch_byte_in_libclang(os.path.join(destdir, 'lib/libclang.so.5.0'),
            0x47aece, b'\x48', b'\x4d')
  else:
    print('Found extracted at {0}'.format(destdir))

def configure(ctx):
  ctx.resetenv(ctx.options.variant)

  ctx.load('compiler_cxx')
  ldflags = []
  if ctx.env.CXX_NAME == 'msvc':
    # /Zi: -g, /WX: -Werror, /W3: roughly -Wall, there is no -std=c++11 equivalent in MSVC.
    # /wd4722: ignores warning C4722 (destructor never returns) in loguru
    # /wd4267: ignores warning C4267 (conversion from 'size_t' to 'type'), roughly -Wno-sign-compare
    # /MD: use multithread c library from DLL
    cxxflags = ['/nologo', '/FS', '/EHsc', '/Zi', '/W3', '/WX', '/wd4996', '/wd4722', '/wd4267', '/wd4800', '/MD']
    if ctx.options.variant == 'debug':
      cxxflags += ['/Zi', '/FS']
      ldflags += ['/DEBUG']
    else:
      cxxflags.append('/O2') # There is no O3
  else:
    if ctx.env.CXXFLAGS:
      cxxflags = ctx.env.CXXFLAGS
    else:
      cxxflags = ['-g', '-Wall', '-Wno-sign-compare', '-Werror']

    if all(not x.startswith('-std=') for x in ctx.env.CXXFLAGS):
      cxxflags.append('-std=c++11')

    if ctx.options.use_clang_cxx:
      # include/clang/Format/Format.h error: multi-line comment
      cxxflags.append('-Wno-comment')
      # Without -fno-rtti, some Clang C++ functions may report `undefined references to typeinfo`
      cxxflags.append('-fno-rtti')

    if ctx.options.variant == 'asan':
      cxxflags.append('-fsanitize=address,undefined')
      cxxflags.append('-O')
      ldflags.append('-fsanitize=address,undefined')
    elif ctx.options.variant == 'debug':
      pass
    else:
      cxxflags.append('-O3')

  ctx.env.CXXFLAGS = cxxflags
  if not ctx.env.LDFLAGS:
    ctx.env.LDFLAGS = ldflags

  ctx.check(header_name='stdio.h', features='cxx cxxprogram', mandatory=True)

  ctx.load('clang_compilation_database', tooldir='.')

  ctx.env['use_system_clang'] = ctx.options.use_system_clang
  ctx.env['use_clang_cxx'] = ctx.options.use_clang_cxx
  ctx.env['llvm_config'] = ctx.options.llvm_config
  ctx.env['bundled_clang'] = ctx.options.bundled_clang
  def libname(lib):
    # Newer MinGW and MSVC both wants full file name
    if sys.platform == 'win32':
      return 'lib' + lib
    return lib
  if ctx.options.use_system_clang:
    # Ask llvm-config for cflags and ldflags
    ctx.find_program(ctx.options.llvm_config, msg='checking for llvm-config', var='LLVM_CONFIG', mandatory=False)
    if ctx.env.LLVM_CONFIG:
      ctx.check_cfg(msg='Checking for clang flags',
                    path=ctx.env.LLVM_CONFIG,
                    package='',
                    uselib_store='clang',
                    args='--cppflags --ldflags')
      # llvm-config does not provide the actual library we want so we check for it
      # using the provided info so far.
      ctx.check_cxx(lib=libname('clang'), uselib_store='clang', use='clang')

    else: # Fallback method using a prefix path
      ctx.start_msg('Checking for clang prefix')
      if not ctx.options.clang_prefix:
        raise ctx.errors.ConfigurationError('not found (--clang-prefix must be specified when llvm-config is not found)')

      prefix = ctx.root.find_node(ctx.options.clang_prefix)
      if not prefix:
        raise ctx.errors.ConfigurationError('clang prefix not found: "%s"'%ctx.options.clang_prefix)

      ctx.end_msg(prefix)

      includes = [ n.abspath() for n in [ prefix.find_node('include') ] if n ]
      libpath  = [ n.abspath() for n in [ prefix.find_node(l) for l in ('lib', 'lib64')] if n ]
      ctx.check_cxx(msg='Checking for library clang', lib=libname('clang'), uselib_store='clang', includes=includes, libpath=libpath)

  else:
    global CLANG_TARBALL_NAME, CLANG_TARBALL_EXT

    if CLANG_TARBALL_NAME is None:
      sys.stderr.write('ERROR: releases.llvm.org does not provide prebuilt binary for your platform {0}\n'.format(sys.platform))
      sys.exit(1)

    # TODO Remove after 5.0.1 is stable
    if ctx.options.bundled_clang == '5.0.0' and sys.platform.startswith('linux'):
      CLANG_TARBALL_NAME = 'clang+llvm-$version-linux-x86_64-ubuntu14.04'

    CLANG_TARBALL_NAME = string.Template(CLANG_TARBALL_NAME).substitute(version=ctx.options.bundled_clang)
    # Directory clang has been extracted to.
    CLANG_DIRECTORY = '{0}/{1}'.format(out, CLANG_TARBALL_NAME)
    # URL of the tarball to download.
    CLANG_TARBALL_URL = 'http://releases.llvm.org/{0}/{1}{2}'.format(ctx.options.bundled_clang, CLANG_TARBALL_NAME, CLANG_TARBALL_EXT)

    print('Checking for clang')
    download_and_extract(CLANG_DIRECTORY, CLANG_TARBALL_URL, CLANG_TARBALL_EXT)

    bundled_clang_dir = os.path.join(out, ctx.options.variant, 'lib', CLANG_TARBALL_NAME)
    try:
      os.makedirs(os.path.dirname(bundled_clang_dir))
    except OSError:
      pass
    clang_dir = os.path.normpath('../../' + CLANG_TARBALL_NAME)
    try:
      if not os.path.exists(bundled_clang_dir):
        os.symlink(clang_dir, bundled_clang_dir, target_is_directory=True)
    except (OSError, NotImplementedError):
      # Copying the whole directory instead.
      print ('shutil.copytree({0}, {1})'.format(os.path.join(out, CLANG_TARBALL_NAME), bundled_clang_dir))
      try:
        shutil.copytree(os.path.join(out, CLANG_TARBALL_NAME), bundled_clang_dir)
      except Exception as e:
        print('Failed to copy tree, ', e)

    clang_node = ctx.path.find_dir(bundled_clang_dir)
    ctx.check_cxx(uselib_store='clang',
                  includes=clang_node.find_dir('include').abspath(),
                  libpath=clang_node.find_dir('lib').abspath(),
                  lib=libname('clang'))

  ctx.msg('Clang includes', ctx.env.INCLUDES_clang)
  ctx.msg('Clang library dir', ctx.env.LIBPATH_clang)

def build(bld):
  cc_files = bld.path.ant_glob(['src/*.cc', 'src/messages/*.cc'])
  if bld.env['use_clang_cxx']:
    cc_files += bld.path.ant_glob(['src/clang_cxx/*.cc'])

  lib = []
  if sys.platform.startswith('linux'):
    lib.append('rt')
    lib.append('pthread')
    lib.append('dl')
  elif sys.platform.startswith('freebsd'):
    # loguru::stacktrace_as_stdstring calls backtrace_symbols
    lib.append('execinfo')
    # sparsepp/spp_memory.h uses libkvm
    lib.append('kvm')

    lib.append('pthread')
    lib.append('thr')
  elif sys.platform == 'darwin':
    lib.append('pthread')

  if bld.env['use_clang_cxx']:
    # -fno-rtti is required for object files using clang/llvm C++ API

    # The order is derived by topological sorting LINK_LIBS in clang/lib/*/CMakeLists.txt
    lib.append('clangFormat')
    lib.append('clangToolingCore')
    lib.append('clangRewrite')
    lib.append('clangAST')
    lib.append('clangLex')
    lib.append('clangBasic')

    # The order is derived from llvm-config --libs core
    lib.append('LLVMCore')
    lib.append('LLVMBinaryFormat')
    lib.append('LLVMSupport')
    lib.append('LLVMDemangle')

    lib.append('ncurses')

  if bld.env['use_system_clang']:
    if bld.env['llvm_config']:
      rpath = str(subprocess.check_output(
        [bld.env['llvm_config'], '--libdir'],
        stderr=subprocess.STDOUT).decode()).strip()
    else:
      rpath = []

    # Use CXX set by --check-cxx-compiler if it is "clang".
    # See https://github.com/jacobdufault/cquery/issues/237
    clang = bld.env.get_flat('CXX')
    if 'clang' not in clang:
      # Otherwise, infer the clang executable path with llvm-config --bindir
      output = str(subprocess.check_output(
          [bld.env['llvm_config'], '--bindir'],
          stderr=subprocess.STDOUT).decode()).strip()
      clang = os.path.join(output, 'clang')

    # Use the detected clang executable to infer resource directory
    # Use `clang -### -xc /dev/null` instead of `clang -print-resource-dir` because the option is unavailable in 4.0.0
    devnull = '/dev/null' if sys.platform != 'win32' else 'NUL'
    output = subprocess.check_output(
        [clang, '-###', '-xc', devnull],
        stderr=subprocess.STDOUT).decode()
    match = re.search(r'"-resource-dir" "([^"]*)"', output, re.M)
    if match:
      default_resource_directory = match.group(1)
    else:
      bld.fatal("Failed to found system clang resource directory.")

  else:
    clang_tarball_name = os.path.basename(os.path.dirname(bld.env['LIBPATH_clang'][0]))
    default_resource_directory = '../lib/{}/lib/clang/{}'.format(clang_tarball_name, bld.env['bundled_clang'])

    if sys.platform.startswith('freebsd') or sys.platform.startswith('linux'):
      rpath = '$ORIGIN/../lib/' + clang_tarball_name + '/lib'
    elif sys.platform == 'darwin':
      rpath = '@loader_path/../lib/' + clang_tarball_name + '/lib'
    elif sys.platform == 'win32':
      rpath = [] # Unsupported
      name = os.path.basename(os.path.dirname(bld.env['LIBPATH_clang'][0]))
      # Poor Windows users' RPATH
      out_clang_dll = os.path.join(bld.path.get_bld().abspath(), 'bin', 'libclang.dll')
      try:
        os.makedirs(os.path.dirname(out_clang_dll))
      except OSError:
        pass
      try:
        dst = os.path.join(bld.path.get_bld().abspath(), 'lib', name, 'bin', 'libclang.dll')
        os.symlink(dst, out_clang_dll)
      except (OSError, NotImplementedError):
        shutil.copy(dst, out_clang_dll)
    else:
      rpath = bld.env['LIBPATH_clang']

  # https://waf.io/apidocs/tools/c_aliases.html#waflib.Tools.c_aliases.program
  bld.program(
      source=cc_files,
      use='clang',
      includes=[
        'src/',
        'third_party/',
        'third_party/doctest/',
        'third_party/loguru/',
        'third_party/msgpack-c/include',
        'third_party/rapidjson/include/',
        'third_party/sparsepp/'] +
        (['libclang'] if bld.env['use_clang_cxx'] else []),
      defines=[
        'LOGURU_WITH_STREAMS=1',
        'LOGURU_FILENAME_WIDTH=18',
        'LOGURU_THREADNAME_WIDTH=13',
        'DEFAULT_RESOURCE_DIRECTORY="' + default_resource_directory + '"'] +
        (['USE_CLANG_CXX=1', 'LOGURU_RTTI=0']
            if bld.env['use_clang_cxx']
            else []),
      lib=lib,
      rpath=rpath,
      target='bin/cquery')

  if not bld.env['use_system_clang'] and sys.platform != 'win32':
    bld.install_files('${PREFIX}/lib/' + clang_tarball_name + '/lib', bld.path.get_bld().ant_glob('lib/' + clang_tarball_name + '/lib/libclang.(dylib|so.[4-9])', quiet=True))
    if bld.cmd == 'install':
      # TODO This may be cached and cannot be re-triggered. Use proper shell escape.
      bld(rule='rsync -rtR {}/./lib/{}/lib/clang/*/include {}/'.format(bld.path.get_bld(), clang_tarball_name, bld.env['PREFIX']))

def init(ctx):
  from waflib.Build import BuildContext, CleanContext, InstallContext, UninstallContext
  for y in (BuildContext, CleanContext, InstallContext, UninstallContext):
    class tmp(y):
      variant = ctx.options.variant

  # This is needed because waf initializes the ConfigurationContext with
  # an arbitrary setenv('') which would rewrite the previous configuration
  # cache for the default variant if the configure step finishes.
  # Ideally ConfigurationContext should just let us override this at class
  # level like the other Context subclasses do with variant
  from waflib.Configure import ConfigurationContext
  class cctx(ConfigurationContext):
    def resetenv(self, name):
      self.all_envs = {}
      self.setenv(name)

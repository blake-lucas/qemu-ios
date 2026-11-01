#!/usr/bin/env python3
"""Pinned static/PIC dependencies for Linux or Bionic; outputs outside checkout."""
import argparse,json,pathlib,subprocess,os,hashlib,urllib.request,tarfile,shutil,re
from cache import digest,tool_version,check_cache
p=argparse.ArgumentParser();p.add_argument('output',type=pathlib.Path);p.add_argument('--abi',choices=['linux','arm64-v8a','x86_64'],default='linux');p.add_argument('--ndk',type=pathlib.Path);p.add_argument('--meson',default='meson');p.add_argument('--only',choices=['ffmpeg']);p.add_argument('--prepare-only',action='store_true');a=p.parse_args()
src=pathlib.Path(__file__).resolve().parents[2];out=a.output.resolve();out.mkdir(parents=True,exist_ok=True);prefix=out/'prefix';env=os.environ.copy()
def run(args,cwd):print(' '.join(map(str,args)),flush=True);subprocess.run(list(map(str,args)),cwd=cwd,env=env,check=True)
packages=json.loads((src/'contrib/portable/dependencies.json').read_text())['packages'];trees={}
for dep in packages:
 archive=out/dep['archive']
 if not archive.exists():urllib.request.urlretrieve(dep['url'],archive)
 if hashlib.sha256(archive.read_bytes()).hexdigest()!=dep['sha256']:raise RuntimeError('checksum mismatch: '+str(archive))
 directory=dep.get('directory') or dep['archive'].replace('.tar.xz','').replace('.tar.gz','').replace('.tar.bz2','')
 tree=out/directory
 if not tree.exists():
  with tarfile.open(archive) as tar:tar.extractall(out,filter='data')
 trees[dep['name']]=tree
if a.prepare_only: print('Verified sources extracted in',out);raise SystemExit(0)
cross=[];arch='x86_64';target='linux';cc='cc';cxx='c++';ar='ar';ranlib='ranlib'
if a.abi!='linux':
 if not a.ndk:raise RuntimeError('--ndk required')
 bin=a.ndk.resolve()/'toolchains/llvm/prebuilt/linux-x86_64/bin';arch='aarch64' if a.abi=='arm64-v8a' else 'x86_64';triple='aarch64-linux-android' if arch=='aarch64' else 'x86_64-linux-android'
 cc=str(bin/(triple+'28-clang'));cxx=cc+'++';ar=str(bin/'llvm-ar');ranlib=str(bin/'llvm-ranlib');target='android'
 env['PATH']=str(bin)+':/usr/bin:/bin:'+env['PATH'];env['ANDROID_NDK_ROOT']=str(a.ndk.resolve())
 crossfile=out/'android.cross'
 crossfile_text=f"[binaries]\nc = '{cc}'\ncpp = '{cxx}'\nar = '{ar}'\nstrip = '{bin/'llvm-strip'}'\npkg-config = 'pkg-config'\n[host_machine]\nsystem = 'android'\ncpu_family = '{arch}'\ncpu = '{arch}'\nendian = 'little'\n[properties]\nneeds_exe_wrapper = true\n[built-in options]\nc_args = ['-fPIC']\ncpp_args = ['-fPIC']\n"
 cross=['--cross-file='+str(crossfile)]
env['CC']=cc;env['CXX']=cxx;env['AR']=ar;env['RANLIB']=ranlib;env['CFLAGS']='-fPIC';env['PKG_CONFIG_LIBDIR']=str(prefix/'lib/pkgconfig');env['PKG_CONFIG_PATH']=str(prefix/'lib/pkgconfig')
recipe={'schema':1,'abi':a.abi,'compiler':tool_version([cc]),'compiler_path':cc,'prefix':str(prefix),'ndk_properties_sha256':digest(a.ndk/'source.properties') if a.ndk else None,'archives':{p['name']:p['sha256'] for p in packages},'recipe_sha256':digest(__file__),'cache_guard_sha256':digest(src/'contrib/portable/cache.py'),'patches':{p.name:digest(p) for p in (src/'contrib/ffmpeg').glob('*.patch')},'meson':tool_version([a.meson]),'ninja':tool_version(['ninja']),'cflags':env['CFLAGS'],'library':'static/PIC'}
markers=[prefix/'lib/libavcodec.a',prefix/'lib/libcrypto.a',prefix/'lib/libglib-2.0.a',out/'glib-build/build.ninja',out/'ffmpeg-build/ffbuild/config.mak',trees['libffi']/'Makefile',trees['pcre2']/'Makefile',trees['openssl']/'Makefile']
check_cache(out,recipe,markers,'dependencies')
if a.abi!='linux': crossfile.write_text(crossfile_text)
def meson(name,opts):
 b=out/(name+'-build')
 if not (b/'build.ninja').exists():run([a.meson,'setup',b,trees[name],'--prefix='+str(prefix),'--libdir=lib','--default-library=static','-Dbuildtype=release','-Db_staticpic=true']+cross+opts,out)
 run(['ninja','-j2','-C',b],out);run(['ninja','-C',b,'install'],out)
# The Android platform supplies libz; advertise only its target linker flag.
if target=='android':
 pcdir=prefix/'lib/pkgconfig';pcdir.mkdir(parents=True,exist_ok=True)
 zheader=(a.ndk.resolve()/'toolchains/llvm/prebuilt/linux-x86_64/sysroot/usr/include/zlib.h').read_text()
 zversion=re.search(r'#define ZLIB_VERSION "([^"]+)"',zheader).group(1)
 (pcdir/'zlib.pc').write_text('Name: zlib\nDescription: Android platform zlib\nVersion: '+zversion+'\nLibs: -lz\nCflags: \n')
# PCRE upstream autotools supports Bionic without a Meson vendored subproject.
if not a.only:
 ffi=trees['libffi']
 if not (ffi/'Makefile').exists():run([ffi/'configure','--prefix='+str(prefix),'--libdir='+str(prefix/'lib'),'--disable-shared','--enable-static','--disable-docs']+(['--host='+triple] if target=='android' else []),ffi)
 run(['make','-j2'],ffi);run(['make','install'],ffi)
 proxy=trees['glib']/'subprojects/proxy-libintl'
 if not proxy.exists():shutil.copytree(trees['proxy-libintl'],proxy)
 pc=trees['pcre2']
 if not (pc/'Makefile').exists():run([pc/'configure','--prefix='+str(prefix),'--disable-shared','--enable-static','--disable-jit']+(['--host='+triple] if target=='android' else []),pc)
 run(['make','-j2'],pc);run(['make','install'],pc)
 meson('glib',['-Dtests=false','-Dinstalled_tests=false','-Ddocumentation=false','-Dman-pages=disabled','-Dselinux=disabled','-Dlibmount=disabled','-Dintrospection=disabled','-Dnls=disabled'])
 meson('pixman',['-Dtests=disabled','-Ddemos=disabled','-Dgtk=disabled'])
 ssl=trees['openssl'];ssl_target='android-arm64' if arch=='aarch64' else 'android-x86_64'
 if not (ssl/'Makefile').exists():run([ssl/'Configure',ssl_target if target=='android' else 'linux-x86_64','--prefix='+str(prefix),'--libdir=lib','no-shared','no-tests','no-module','-fPIC']+(['-D__ANDROID_API__=28'] if target=='android' else []),ssl)
 run(['make','-j2'],ssl);run(['make','install_sw'],ssl)
ff=trees['ffmpeg'];fb=out/'ffmpeg-build';fb.mkdir(exist_ok=True)
if not (ff/'.lighttouch-patched').exists():
 for patch in ['h264-chunk-er.patch','h264-cavlc-pcm-offset.patch']:
  with open(src/'contrib/ffmpeg'/patch,'rb') as stream:subprocess.run(['patch','-p1'],cwd=ff,stdin=stream,check=True)
 (ff/'.lighttouch-patched').touch()
if not (fb/'config.mak').exists():
 run([ff/'configure','--prefix='+str(prefix),'--libdir='+str(prefix/'lib'),'--cc='+cc,'--cxx='+cxx,'--ar='+ar,'--ranlib='+ranlib,'--target-os='+('android' if target=='android' else 'linux'),'--arch='+arch,'--disable-everything','--disable-autodetect','--disable-programs','--disable-doc','--disable-x86asm','--disable-avdevice','--disable-avformat','--disable-avfilter','--disable-swscale','--disable-swresample','--enable-decoder=aac,mp3,alac,h264','--enable-encoder=aac','--enable-static','--disable-shared','--enable-pic']+(['--enable-cross-compile'] if target=='android' else []),fb)
run(['make','-j2'],fb);run(['make','install'],fb)
(out/'dependency-manifest.json').write_text(json.dumps({'schema':1,'abi':a.abi,'packages':packages,'recipe':recipe,'built_packages':['ffmpeg'] if a.only else [p['name'] for p in packages],'ndk':str(a.ndk.resolve()) if a.ndk else None,'compiler':subprocess.check_output([cc,'--version'],env=env,text=True).splitlines()[0]},indent=2)+'\n')
print(prefix)

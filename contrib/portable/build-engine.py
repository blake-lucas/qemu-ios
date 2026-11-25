#!/usr/bin/env python3
"""Build a reusable QEMU TCG engine; one worker process per qemu_ios_main."""
import argparse,pathlib,os,subprocess,shutil,importlib.util
from cache import digest,tool_version,check_cache
spec=importlib.util.spec_from_file_location('engine_cache',pathlib.Path(__file__).with_name('engine-cache.py'));guard=importlib.util.module_from_spec(spec);spec.loader.exec_module(guard)
p=argparse.ArgumentParser();p.add_argument('output',type=pathlib.Path);p.add_argument('--abi',choices=['linux','arm64-v8a','x86_64'],default='linux');p.add_argument('--prefix',type=pathlib.Path,required=True);p.add_argument('--crypto-prefix',type=pathlib.Path,required=True);p.add_argument('--ndk',type=pathlib.Path);p.add_argument('--configure-only',action='store_true');p.add_argument('--source-update',action='store_true',help='Allow a clean source revision change with identical configuration/tools/dependencies');p.add_argument('--android-api',type=int,choices=[28,29],help='Engine minimum Android API (default 29, the app minimum: native ELF TLS; 28 falls back to emulated TLS, ~4%% slower on a Poco F3). Changing it needs a fresh build cache');p.add_argument('--cpp-standard',choices=['gnu++17','gnu++23'],help='Optional C++ dialect override for older Linux baseline toolchains; default follows upstream');p.add_argument('--cflags',default='',help='More C flags (e.g. -fprofile-generate for a desktop PGO profile run, or -fprofile-use=DIR)');p.add_argument('--ldflags',default='',help='More link flags, as --cflags');p.add_argument('--experimental-tb-cache-clear',action='store_true',help='Fresh Linux-only clear-operation diagnostic; default4096, no lookup counters or geometry change');a=p.parse_args()
if a.abi=='linux' and a.android_api is not None:p.error('--android-api is an Android engine option')
if a.android_api is None:a.android_api=29
if a.experimental_tb_cache_clear and (a.abi!='linux' or a.source_update or a.output.exists() or a.output.is_symlink()):p.error('--experimental-tb-cache-clear requires Linux, no source-update, and an absent fresh output')
src=pathlib.Path(__file__).resolve().parents[2];out=a.output.resolve();out.mkdir(parents=True,exist_ok=True);prefix=a.prefix.resolve();env=os.environ.copy()
crypto_prefix=a.crypto_prefix.resolve()
env['PKG_CONFIG_PATH']=str(crypto_prefix/'lib/pkgconfig')+':'+str(prefix/'lib/pkgconfig')
if a.abi!='linux': env['PKG_CONFIG_LIBDIR']=env['PKG_CONFIG_PATH']
pc=out/'target-pkg-config'
pc.write_text('#!/bin/sh\nexec pkg-config --static "$@"\n');pc.chmod(0o755);env['PKG_CONFIG']=str(pc)
args=[src/'configure','--target-list=arm-softmmu','--without-default-features','--without-default-devices','--with-devices-arm=portable','--enable-tcg','--enable-pixman','--enable-gcrypt','--enable-slirp','--python=/usr/bin/python3','--disable-docs','--disable-tools','--disable-werror','--disable-pie','--extra-cflags=-fPIC -DQEMU_IOS_PORTABLE_CRYPTO','--extra-ldflags=-L'+str(prefix/'lib'),'--audio-drv-list=']
if a.cpp_standard:
 if a.abi!='linux':p.error('--cpp-standard is a Linux packaging option')
 args+=['-Dcpp_std='+a.cpp_standard]
if a.cflags:args[args.index('--extra-cflags=-fPIC -DQEMU_IOS_PORTABLE_CRYPTO')]+=' '+a.cflags
if a.ldflags:args=[x+' '+a.ldflags if str(x).startswith('--extra-ldflags=') else x for x in args]
if a.experimental_tb_cache_clear:
 args[args.index('--extra-cflags=-fPIC -DQEMU_IOS_PORTABLE_CRYPTO')]+=' -DQEMU_EXPERIMENTAL_TB_CACHE_STATS=1'
if a.abi!='linux':
 if not a.ndk:raise RuntimeError('--ndk required')
 bin=a.ndk.resolve()/'toolchains/llvm/prebuilt/linux-x86_64/bin';arch='aarch64' if a.abi=='arm64-v8a' else 'x86_64';triple='aarch64-linux-android' if arch=='aarch64' else 'x86_64-linux-android'
 env['PATH']=str(bin)+':'+env['PATH']
 for key,name in [('AR','llvm-ar'),('RANLIB','llvm-ranlib'),('NM','llvm-nm'),('STRIP','llvm-strip'),('READELF','llvm-readelf')]:env[key]=str(bin/name)
 args+=['--cross-prefix=','--host-cc=/usr/bin/cc','--cpu='+arch,'--cc='+str(bin/(triple+str(a.android_api)+'-clang')),'--cxx='+str(bin/(triple+str(a.android_api)+'-clang++')),'--disable-containers']
revision=subprocess.check_output(['git','rev-parse','HEAD'],cwd=src,text=True).strip()
status=subprocess.check_output(['git','status','--porcelain=v1','--untracked-files=all'],cwd=src,text=True)
if status:raise RuntimeError('Commit source changes before a reproducible engine build: '+status)
cc=next((str(arg).split('=',1)[1] for arg in args if str(arg).startswith('--cc=')),env.get('CC','cc'))
cxx=next((str(arg).split('=',1)[1] for arg in args if str(arg).startswith('--cxx=')),env.get('CXX','c++'))
tools={'compiler_path':str(pathlib.Path(shutil.which(cc)).resolve()),'compiler_version':tool_version([cc]),'compiler_sha256':digest(shutil.which(cc)),'cxx_path':str(pathlib.Path(shutil.which(cxx)).resolve()),'cxx_version':tool_version([cxx]),'cxx_sha256':digest(shutil.which(cxx)),'ninja_version':tool_version(['ninja']),'ninja_sha256':digest(shutil.which('ninja'))}
recipe={'schema':2,'abi':a.abi,'source_revision':revision,'argv':list(map(str,args)),'recipe_sha256':digest(__file__),'cache_guard_sha256':digest(src/'contrib/portable/cache.py'),'source_update_guard_sha256':digest(src/'contrib/portable/engine-cache.py'),'tools':tools,'environment':{key:env.get(key,'') for key in ['CC','CXX','CFLAGS','CXXFLAGS','CPPFLAGS','LDFLAGS']},'dependency_manifest_sha256':digest(prefix.parent/'dependency-manifest.json'),'crypto_dependency_manifest_sha256':digest(crypto_prefix.parent/'dependency-manifest.json'),'ndk_properties_sha256':digest(a.ndk/'source.properties') if a.ndk else None}
guard.check_engine_cache(out,recipe,[out/'build.ninja',out/'compile_commands.json',out/'config-host.mak'],a.source_update)
# Build scripts use explicit argv, never shell evaluation. No private assets.
print(' '.join(map(str,args)),flush=True);subprocess.run(list(map(str,args)),cwd=out,env=env,check=True)
if not a.configure_only:
 subprocess.run(['ninja','-j'+os.environ.get('LT_JOBS','2'),'-d','keeprsp','qemu-system-arm'],cwd=out,env=env,check=True)
 if a.abi!='linux':shutil.copyfile(a.ndk.resolve()/'toolchains/llvm/prebuilt/linux-x86_64/sysroot/usr/lib'/triple/'libc++_shared.so',out/'libc++_shared.so')
 subprocess.run(['python3',str(src/'contrib/portable/make-shared.py'),str(out)],cwd=out,env=env,check=True)

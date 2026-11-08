import importlib.util, os, pathlib, subprocess, sys, tempfile

base=pathlib.Path(__file__).resolve().parent
source=pathlib.Path(sys.argv[1]).resolve() if len(sys.argv)>1 else base/'ios-native-lf'
spec=importlib.util.spec_from_file_location('builder',source/'tools/build-fuxi-nativemetal.py')
builder=importlib.util.module_from_spec(spec); spec.loader.exec_module(builder)
os.environ.update(GIT_CONFIG_COUNT='1',GIT_CONFIG_KEY_0='core.autocrlf',GIT_CONFIG_VALUE_0='false')
with tempfile.TemporaryDirectory(prefix='native-patch-regression-') as tmp:
    root=pathlib.Path(tmp)/'source'; root.mkdir(); patch=pathlib.Path(tmp)/'change.patch'
    def git(*args):
        return subprocess.run(['git','-C',str(root),*args],check=True,stdout=subprocess.PIPE,stderr=subprocess.PIPE).stdout
    git('init','-q'); git('config','user.name','Test'); git('config','user.email','test@example.invalid')
    tracked=root/'tracked.txt'; added=root/'new.txt'; tracked.write_bytes(b'original\n')
    git('add','tracked.txt'); git('commit','-qm','fixture')
    tracked.write_bytes(b'updated\n'); added.write_bytes(b'new file\n'); git('add','tracked.txt','new.txt')
    patch.write_bytes(git('diff','--cached','--binary','HEAD'))
    git('reset','-q','HEAD'); tracked.write_bytes(b'original\n'); added.unlink()
    builder.apply_stack(root,[patch]); builder.apply_stack(root,[patch])
    assert tracked.read_bytes()==b'updated\n' and added.read_bytes()==b'new file\n'
    assert not git('diff','--cached','--name-only')
    def rejected():
        try: builder.apply_stack(root,[patch])
        except (SystemExit,subprocess.CalledProcessError): return
        raise AssertionError('unexpected source modification accepted')
    added.write_bytes(b'tampered\n'); rejected(); added.write_bytes(b'new file\n')
    extra=root/'extra.txt'; extra.write_bytes(b'unexpected\n'); rejected(); extra.unlink()
    tracked.write_bytes(b'tampered\n'); rejected(); tracked.write_bytes(b'updated\n')
    git('add','tracked.txt'); rejected(); git('reset','-q','HEAD','--','tracked.txt')
    builder.apply_stack(root,[patch])
    print('PASS: new-file patch, idempotence, unmodified real index, and tamper/untracked/staged rejection')

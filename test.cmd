@echo on
set PATH=%PATH%;%prefixPath%\bin
pushd build
ninja test
popd

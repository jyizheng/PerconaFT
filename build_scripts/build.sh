CC=gcc CXX=g++ cmake \
    -D CMAKE_BUILD_TYPE=Debug \
    -D BUILD_TESTING=OFF \
    -D USE_VALGRIND=OFF \
    -D CMAKE_INSTALL_PREFIX=../prefix/ \
    ..
cmake --build . --target install


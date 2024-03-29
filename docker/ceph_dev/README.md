# Ceph Builder Image

This docker image contains build dependencies for Ceph development,
based on the builder image hosted at:
https://github.com/systemslab/docker-cephdev/blob/master/builder-base/Dockerfile.

**NOTE:** this Dockerfile is supposed to be used by ICM to build the Docker
image on ByteCloud. If you want to build the Docker image locally via
`docker build .`, you need to change the line:

    ADD ./docker/ceph_dev/assets /build/

into:

    ADD ./assets /build/

This is needed because ICM always use the top-level directory of the Git
repository as the [build context](https://docs.docker.com/engine/reference/commandline/build/).

Alternatively, you can run the `docker build` command from the top-level
directory:

    docker build -f docker/ceph_dev/Dockerfile .
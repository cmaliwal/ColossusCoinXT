### Setup docker on Debian 8.1

Install notes: https://docs.docker.com/install/linux/docker-ce/debian/#install-using-the-repository

Docker CLI: https://docs.docker.com/engine/reference/run/

Script that installs docker-ce: `setup_docker_debian.sh`

### Build&Run COLX docker container

Build container: `sudo docker build --no-cache --tag colx .`

Run container: `sudo docker run -d --name colx.cont colx`

Start container: `sudo docker start colx.cont`

See if it is up: `sudo docker ps -a`

Shell in the container: `sudo docker exec -it colx.cont /bin/bash`

Test RPC (shell inside container): `colx-cli help` or `colx-cli getinfo`

Stop container: `sudo docker stop colx.cont`

Delete container: `sudo docker rm colx.cont`
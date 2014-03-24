#! /bin/bash
subnet=192.168.122
container_prefix=lxc
domain_suffix=example.com

usage() 
{
    echo "Description: create multiple containers."
    echo "usage: $0 <options>"
    echo "  -n: number of containers to be created"
    echo "  -s: the start number"
    exit 1
}

get_seq()
{
    local count=$1
    local end=$(( $start + $count - 1 ))
    seq -s " " $start $end
}

# main - here we begin
[ $# -eq 0 ] && usage

num_machine=$1
start=100
while [ $# -gt 0 ]; do
   case $1 in
       -n) num_machine=$2; shift 2;;
       -s) start=$2; shift 2;;
       *) echo "Invalid option $1." && usage;;
   esac
done

# create a temp file to store the mappings
tmpfile=$(mktemp)

# create the containers
for idx in $(get_seq $num_machine);do
    container_name=${container_prefix}$idx
    fqdn=$container_name.${domain_suffix}
    ip=${subnet}.$idx

    # check if the container exists
    lxc-info -n $container_name > /dev/null 2>&1
    if [ $? -eq 0 ];then
        echo "Container $container_name exists. Skip creating it."
        continue
    fi

    echo "Creating container $container_name..."
    root_password="welcome" lxc-create -t centos -n $container_name -- --ip $ip --gw ${subnet}.1 --fqdn $fqdn -E -s

    # append the mapping to /etc/hosts
    grep $container_name /etc/hosts > /dev/null 2>&1 || echo "$ip $fqdn $container_name" >> /etc/hosts

    # create a copy in tmpfile as well
    echo "$ip $fqdn $container_name" >> $tmpfile
done

# enable the containers can resolve to each other
for idx in $(get_seq $num_machine);do
    container_name=${container_prefix}$idx
    cat $tmpfile >> /usr/local/var/lib/lxc/$container_name/rootfs/etc/hosts
done

rm -f $tmpfile

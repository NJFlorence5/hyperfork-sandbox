# 1. Create a 105GB sparse file (takes 0 seconds, uses almost 0 disk space initially)
fallocate -l 105G $(pwd)/vm_files/cow_storage.img

# 2. Format it as XFS (which natively supports reflinks)
mkfs.xfs $(pwd)/vm_files/cow_storage.img

# 3. Create a mount point and mount the loopback device
mkdir -p $(pwd)/vm_files/cow_mount
mount -o loop $(pwd)/vm_files/cow_storage.img $(pwd)/vm_files/cow_mount

# 4. Move your 100GB base image into the new XFS mount
mv $(pwd)/vm_files/ubuntu-20-hyperfork.raw $(pwd)/vm_files/cow_mount/

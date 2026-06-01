使用官方
![alt text](image.png)

![alt text](image-1.png)

![alt text](image-2.png)


然后连接就可以使用ffmpeg进行拍照操作



需要手动创建一个ext4,并且为mbr活动分区
我存在使用debug模式一直报错group descripter:ext4groupdisc，不确定是不是debug模式的原因，并且agent说编译的uimg里面并没有使用第三个分区的相关bootarg,没编译进去
直接rebase,重新操作就解决了，不知道原因

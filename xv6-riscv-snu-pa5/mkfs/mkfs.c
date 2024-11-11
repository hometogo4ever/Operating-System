#include <stdio.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <assert.h>

#define stat xv6_stat  // avoid clash with host struct stat
#include "kernel/types.h"
#include "kernel/fs.h"
#include "kernel/stat.h"
#include "kernel/param.h"

#ifndef static_assert
#define static_assert(a, b) do { switch (0) case 0: case (a): ; } while (0)
#endif

#ifndef SNU
// NINODES moved to kernel/param.h
#define NINODES 200
#endif

// Disk layout:
// [ boot block | sb block | log | inode blocks | free bit map | data blocks ]
/*
int nbitmap = FSSIZE/(BSIZE*8) + 1;
int ninodeblocks = NINODES / IPB + 1;
int nlog = LOGSIZE;
int nmeta;    // Number of meta blocks (boot, sb, nlog, inode, bitmap)
int nblocks;  // Number of data blocks
*/

// FAT Disk Layout:
// [ boot block | sb block | log | fat blocks | inode blocks | data blocks]
int ninodeblocks = NINODES / IPB + 1;
int nlog = LOGSIZE;
int nfat = 8;
int nmeta;
int nblocks;

int fsfd;
struct superblock sb;
char zeroes[BSIZE];
uint freeinode = 1;
uint freeblock;

void falloc();
uint ifat();
//void balloc(int);
void wsect(uint, void*);
void winode(uint, struct dinode*);
void rinode(uint inum, struct dinode *ip);
void rsect(uint sec, void *buf);
uint ialloc(ushort type);
void iappend(uint inum, void *p, int n);
void die(const char *);
uint fatupdate(uint);

// convert to riscv byte order
ushort
xshort(ushort x)
{
  ushort y;
  uchar *a = (uchar*)&y;
  a[0] = x;
  a[1] = x >> 8;
  return y;
}

uint
xint(uint x)
{
  uint y;
  uchar *a = (uchar*)&y;
  a[0] = x;
  a[1] = x >> 8;
  a[2] = x >> 16;
  a[3] = x >> 24;
  return y;
}

int
main(int argc, char *argv[])
{
  int i, cc, fd;
  uint rootino, inum, off;
  struct dirent de;
  char buf[BSIZE];
  struct dinode din;


  static_assert(sizeof(int) == 4, "Integers must be 4 bytes!");

  if(argc < 2){
    fprintf(stderr, "Usage: mkfs fs.img files...\n");
    exit(1);
  }

  assert((BSIZE % sizeof(struct dinode)) == 0);
  assert((BSIZE % sizeof(struct dirent)) == 0);

  fsfd = open(argv[1], O_RDWR|O_CREAT|O_TRUNC, 0666);
  if(fsfd < 0)
    die(argv[1]);

  // 1 fs block = 1 disk sector
  nmeta = 2 + nlog + ninodeblocks + nfat; //TODO : CHANGE META SIZE
  nblocks = FSSIZE - nmeta;

  sb.magic = FSMAGIC_FATTY; //TODO : CHANGE MAGIC
  sb.size = xint(FSSIZE);
  sb.nblocks = xint(nblocks);
  sb.ninodes = xint(NINODES);
  sb.nlog = xint(nlog);
  sb.nfat = xint(nfat);
  sb.fatstart = xint(2+nlog);
  sb.freehead = 44;
  sb.freeblks = 0;
  sb.logstart = xint(2);
  sb.inodestart = xint(2+nlog+nfat);
  //sb.bmapstart = xint(2+nlog+ninodeblocks); // TODO: NO BITMAP
  //TODO : FAT META DATA IS NEEDED

  

  freeblock = nmeta;     // the first free block that we can allocate


  for(i = 0; i < FSSIZE; i++)
    wsect(i, zeroes);
  
  falloc();


  rootino = ialloc(T_DIR);
  assert(rootino == ROOTINO);

  bzero(&de, sizeof(de));
  de.inum = xshort(rootino);
  strcpy(de.name, ".");
  iappend(rootino, &de, sizeof(de));

  bzero(&de, sizeof(de));
  de.inum = xshort(rootino);
  strcpy(de.name, "..");
  iappend(rootino, &de, sizeof(de));
  for(i = 2; i < argc; i++){
    // get rid of "user/"
    char *shortname;
    if(strncmp(argv[i], "user/", 5) == 0)
      shortname = argv[i] + 5;
    else
      shortname = argv[i];
    
    assert(index(shortname, '/') == 0);

    if((fd = open(argv[i], 0)) < 0) 
      die(argv[i]);

    // Skip leading _ in name when writing to file system.
    // The binaries are named _rm, _cat, etc. to keep the
    // build operating system from trying to execute them
    // in place of system binaries like rm and cat.
    if(shortname[0] == '_')
      shortname += 1;
    inum = ialloc(T_FILE);
    bzero(&de, sizeof(de));
    de.inum = xshort(inum);
    strncpy(de.name, shortname, DIRSIZ);
    iappend(rootino, &de, sizeof(de));
    while((cc = read(fd, buf, sizeof(buf))) > 0)
      iappend(inum, buf, cc);
    close(fd);
  }
  // fix size of root inode dir
  rinode(rootino, &din);
  off = xint(din.size);
  off = ((off/BSIZE) + 1) * BSIZE;
  din.size = xint(off);
  winode(rootino, &din);
  memset(buf, 0, sizeof(buf));
  memmove(buf, &sb, sizeof(sb));
  wsect(1, buf);
  printf("freeblk : %d, freehead : %d\n", sb.freeblks, sb.freehead);
  exit(0);
}

// Write data to Sector : To sector 'sec', write data from 'buf'
void 
wsect(uint sec, void *buf)
{
  if(lseek(fsfd, sec * BSIZE, 0) != sec * BSIZE)
    die("lseek");
  if(write(fsfd, buf, BSIZE) != BSIZE) // write data block from buf to fsfd
    die("write");
}

// Write new Inode ip to Inode 'inum'
void
winode(uint inum, struct dinode *ip)
{
  char buf[BSIZE];
  uint bn;
  struct dinode *dip;

  bn = IBLOCK(inum, sb); // Inode Block of inum
  rsect(bn, buf); // Read That Inode Block data to buf
  dip = ((struct dinode*)buf) + (inum % IPB); // Inode block referenced
  *dip = *ip; //set that block to ip
  wsect(bn, buf); // write back to disk
}

// Read Inode 'inum' to ip
void
rinode(uint inum, struct dinode *ip)
{
  char buf[BSIZE];
  uint bn;
  struct dinode *dip;

  bn = IBLOCK(inum, sb);
  rsect(bn, buf);
  dip = ((struct dinode*)buf) + (inum % IPB);
  *ip = *dip;
}

// Read Sector 'sec' to 'buf'
void
rsect(uint sec, void *buf)
{
  if(lseek(fsfd, sec * BSIZE, 0) != sec * BSIZE)
    die("lseek");
  if(read(fsfd, buf, BSIZE) != BSIZE)
    die("read");
}

// allocate new inode, type is 'type'
uint
ialloc(ushort type)
{
  uint inum = freeinode++; // new inode number : linear increament
  //printf("new inode : %d\n", inum);
  struct dinode din;

  bzero(&din, sizeof(din)); // set din to 0 : initialize
  din.type = xshort(type); // set inode type
  din.nlink = xshort(1); // set inode link : initially 1
  din.size = xint(0); // set size : initially 0
  din.startblk = ifat();
  winode(inum, &din); // Write this inode to 'inum' inode
  return inum; // return new inode number
}

/*
// This is bitmap block allocator : Let's Remove.
// TODO : REMOVE bmapstart
void
balloc(int used)
{
  uchar buf[BSIZE];
  int i;

  printf("balloc: first %d blocks have been allocated\n", used);
  assert(used < BSIZE*8);
  bzero(buf, BSIZE);
  for(i = 0; i < used; i++){
    buf[i/8] = buf[i/8] | (0x1 << (i%8));
  }
  printf("balloc: write bitmap block at sector %d\n", sb.bmapstart);
  wsect(sb.bmapstart, buf);
} */
uint
ifat ()
{
  char buf[BSIZE];
  rsect(xint(sb.fatstart + sb.freehead/256), buf);
  uint * fatnum = (uint*) buf;
  uint temp = sb.freehead;
  sb.freehead = fatnum[sb.freehead%256];
  fatnum[temp%256] = 0;
  wsect(xint(sb.fatstart + temp/256), buf);
  sb.freeblks--;
  printf("initial after : freeblk %d / freehead %d\n", sb.freeblks, sb.freehead);
  return temp;
}

uint
fatupdate(uint last)
{
  char buf[BSIZE];
  rsect(xint(sb.fatstart + last/256), buf);
  uint *fatnum = (uint*)buf;
  if (fatnum[last%256]) {// If this is not final block
    //printf("existing\n");
    return fatnum[last%256];
  }
  else { // this is final block : additional allocation
    fatnum[last%256] = sb.freehead;
    //printf("updated : %d to block %d\n", fatnum[last%256], sb.fatstart + last/256);
    wsect(xint(sb.fatstart + last/256), buf);
    //printf("next num : %d to block %d\n",sb.freehead, sb.fatstart + sb.freehead/256);
    rsect(xint(sb.fatstart + sb.freehead/256), buf);
    fatnum = (uint*)buf;
    uint rtn = sb.freehead;
    //printf("read : from %d rd %d + rtn : %d\n", sb.freehead%256, fatnum[sb.freehead%256], rtn);
    sb.freehead = fatnum[rtn%256];
    fatnum[rtn%256] = 0;
    wsect(xint(sb.fatstart + rtn/256), buf);
    sb.freeblks--;
    printf("after : freeblk %d / freehead %d\n", sb.freeblks, sb.freehead);
    return rtn;
  }
  /*
  rsect(xint(sb.fatstart + last/256), buf);
  uint *fatnum = (uint*)buf;
  fatnum[last%256] = sb.freehead;
  wsect(xint(sb.fatstart + last/256), buf);

  rsect(xint(sb.fatstart + sb.freehead/256), buf);
  fatnum = (uint*)buf;
  printf("next value : %d\n", fatnum[sb.freehead%256]);

  
  uint temp = sb.freehead;
  sb.freehead = fatnum[sb.freehead%256];
  fatnum[sb.freehead%256] = 0;
  wsect(xint(sb.fatstart + sb.freehead/256), buf);
  sb.freeblks--;

  return temp;*/
}

void
falloc()
{
  char  buf[BSIZE];
  int i;
  int j = 0;

  //printf("falloc : start\n");
  for (i=0;i<8;i++) {
    rsect(xint(sb.fatstart + i), buf);
    uint *fatnum = (uint*)buf;
    for (int k=0;k<256;k++) {
      if (j  < 44)
        fatnum[k] = -1;
      else if (j >= FSSIZE)
        fatnum[k] = 0;
      else {
        fatnum[k] = j+1;
        sb.freeblks++;
      }
      j++;
    }
    wsect(xint(sb.fatstart + i), buf);
  }
  //printf("falloc end\n");

}

#define min(a, b) ((a) < (b) ? (a) : (b))

// add data from xp to inode inum
//TODO : REMOVE addr, use start block instead
void
iappend(uint inum, void *xp, int n)
{
  //printf("inode : %d size: %d\n", inum, n);
  char *p = (char*)xp;
  uint fbn, off, n1, sz;
  struct dinode din;
  char buf[BSIZE];

  rinode(inum, &din); // read inode 'inum' to din
  sz = xint(din.size);
  uint off1 = xint(din.startblk);
  fbn = off1/256 + sb.fatstart;
  rsect(xint(fbn), buf);
  uint *fatnum = (uint *)buf;

  uint fst, snd;
  fst = off1; // data block offset
  // Find new block to append
  while (1) {
    if (fbn-sb.fatstart != (fst/256)) {
      //printf("NNN\n");
      fbn = fst/256 + sb.fatstart;
      rsect(xint(fbn), buf);
      fatnum = (uint*)buf;
    }
    snd = fatnum[fst%256];
    if (snd == 0)
      break;
    else 
      fst = snd;
  }
  while (n>0) {
    if (sz > 0 && sz%BSIZE == 0) {
      off = fatupdate(fst);
    }
    else {
      off = fst;
    }
    //printf("%d\n", off);
    rsect(xint(off), buf);
    n1 = min(n, BSIZE-sz%BSIZE);
    bcopy(p, buf + sz % BSIZE, n1);
    wsect(xint(off), buf);
    p += n1;
    n -= n1;
    sz += n1;
  }
  din.size = xint(sz);
  winode(inum, &din);
}

void
die(const char *s)
{
  perror(s);
  exit(1);
}

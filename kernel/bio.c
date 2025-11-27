// Buffer cache.
//
// The buffer cache is a linked list of buf structures holding
// cached copies of disk block contents.  Caching disk blocks
// in memory reduces the number of disk reads and also provides
// a synchronization point for disk blocks used by multiple processes.
//
// Interface:
// * To get a buffer for a particular disk block, call bread.
// * After changing buffer data, call bwrite to write it to disk.
// * When done with the buffer, call brelse.
// * Do not use the buffer after calling brelse.
// * Only one process at a time can use a buffer,
//     so do not keep them longer than necessary.


#include <time.h>

#include "types.h"
#include "param.h"
#include "spinlock.h"
#include "sleeplock.h"
#include "riscv.h"
#include "defs.h"
#include "fs.h"
#include "buf.h"

// struct {
//   struct spinlock lock;
//   struct buf buf[NBUF];
//
//   // Linked list of all buffers, through prev/next.
//   // Sorted by how recently the buffer was used.
//   // head.next is most recent, head.prev is least.
//   struct buf head;
// } bcache;

#define NBUFMAP_BUCKET 13
#define BUFMAP_HASH(dev,blockno) (((dev)<<27 |(blockno))%NBUFMAP_BUCKET)

struct
{
  struct buf buf[NBUF];
  struct spinlock eviction_lock;             // 驱逐锁
  struct spinlock bufmap_locks[NBUFMAP_BUCKET];   //桶锁
  struct buf bufmap[NBUFMAP_BUCKET];
}bcache;

void
binit(void)
{
  //初始化桶
  for (int i =0; i < NBUFMAP_BUCKET; i++)
  {
    initlock(&bcache.bufmap_locks[i] , "bufmap_locks");
    bcache.bufmap[i].next = 0;
  }
  for (int i =0; i < NBUF; i++)
  {
    struct buf *b = &bcache.buf[i];
    initsleeplock(&b->lock , "buf_locks");
    b->refcnt = 0;
    b->lastuse = 0;
    //将缓冲区块添加到桶0中
    b->next = bcache.bufmap[0].next;
    bcache.bufmap[0].next = b;
  }
  initlock(&bcache.eviction_lock , "eviction_lock");
}

// Look through buffer cache for block on device dev.
// If not found, allocate a buffer.
// In either case, return locked buffer.
static struct buf*
bget(uint dev, uint blockno)
{
  struct buf *b;
  uint key = BUFMAP_HASH(dev,blockno);
  acquire(&bcache.bufmap_locks[key]);
  //如果对应桶找到
  for (b = bcache.bufmap[key].next ; b ; b = b->next)
  {
    if (b->dev == dev && b->blockno == blockno)
    {
      b->refcnt++;
      release(&bcache.bufmap_locks[key]);
      acquiresleep(&b->lock);
      return b;
    }
  }
  //遍历所有桶，同时添加全局锁和桶锁
  release(&bcache.bufmap_locks[key]);
  acquire(&bcache.eviction_lock);
  //防止释放间隙，出现其他线程写入
  for (b = bcache.bufmap[key].next ; b ; b = b->next)
  {
    if (b->dev == dev && b->blockno == blockno)
    {
      acquire(&bcache.bufmap_locks[key]);
      b->refcnt++;
      release(&bcache.bufmap_locks[key]);
      release(&bcache.eviction_lock);
      acquiresleep(&b->lock);
      return b;
    }
  }
  //开始查询所有桶lru buf
  uint hold_bucket = -1;     //记录哪个桶持有锁
  struct buf* before_least = 0;    // 记录lru buf 前一块
  for (int i =0; i < NBUFMAP_BUCKET; i++)
  {
    acquire(&bcache.bufmap_locks[i]);
    uint newfound = 0;      //是否在当前桶找到新的lru buf
    for (b = bcache.bufmap[i].next ; b ; b = b->next)
    {
      if ((b->refcnt == 0) && ((!before_least) || (b->lastuse < before_least->next->lastuse)))
      {
        before_least = b;
        newfound = 1;
      }
      if (!newfound)            //没有找到要释放当前桶
      {
        release(&bcache.bufmap_locks[i]);
      }
      else
      {
        if (hold_bucket != -1)             //如果找到和之前不同桶，需要释放之前桶
        {
          release(&bcache.bufmap_locks[hold_bucket]);
        }
        hold_bucket = i;                  //并且记录当前桶
      }
    }
  }
  if (!before_least)  // 没有空间缓存快
  {
    panic("bget : no buffers");
  }
  b = before_least->next;
  if (hold_bucket != key)
  {
    before_least->next = b->next;   //从桶中移除
    release(&bcache.bufmap_locks[hold_bucket]);
    //添加到key中
    acquire(&bcache.bufmap_locks[key]);
    b->next = bcache.bufmap[key].next;
    bcache.bufmap[key].next = b;
  }
  //赋值
  b->dev = dev;
  b->blockno = blockno;
  b->refcnt = 1;
  b->valid = 0;
  release(&bcache.bufmap_locks[key]);
  release(&bcache.eviction_lock);
  acquiresleep(&b->lock);
  return b;
}

// Return a locked buf with the contents of the indicated block.
struct buf*
bread(uint dev, uint blockno)
{
  struct buf *b;

  b = bget(dev, blockno);
  if(!b->valid) {
    virtio_disk_rw(b, 0);
    b->valid = 1;
  }
  return b;
}

// Write b's contents to disk.  Must be locked.
void
bwrite(struct buf *b)
{
  if(!holdingsleep(&b->lock))
    panic("bwrite");
  virtio_disk_rw(b, 1);
}

// Release a locked buffer.
// Move to the head of the most-recently-used list.
void
brelse(struct buf *b)
{
  if (!holdingsleep(&b->lock))
    panic("brelse");
  releasesleep(&b->lock);

  uint key = BUFMAP_HASH(b->dev,b->blockno);
  acquire(&bcache.bufmap_locks[key]);
  b->refcnt--;
  if (b->refcnt == 0)
    b->lastuse = ticks;
  release(&bcache.bufmap_locks[key]);
}

void
bpin(struct buf *b) {
  uint key = BUFMAP_HASH(b->dev,b->blockno);
  acquire(&bcache.bufmap_locks[key]);
  b->refcnt++;
  release(&bcache.bufmap_locks[key]);
}

void
bunpin(struct buf *b) {
  uint key = BUFMAP_HASH(b->dev,b->blockno);
  acquire(&bcache.bufmap_locks[key]);
  b->refcnt--;
  release(&bcache.bufmap_locks[key]);
}



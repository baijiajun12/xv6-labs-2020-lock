struct buf {
  int valid;   // has data been read from disk?
  int disk;    // does disk "own" buf?
  uint dev;
  uint blockno;
  struct sleeplock lock;
  uint refcnt;
  uint lastuse;   //用于跟踪lru
  struct buf *next;
  uchar data[BSIZE];
};


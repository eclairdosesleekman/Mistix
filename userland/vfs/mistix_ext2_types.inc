{ Phase 8 userland Ext2 client: read-only Ext2 over blockd RPC.
  Supports 1/2/4 KiB blocks, primary MBR Linux partition discovery,
  linear directory traversal, direct blocks and single-indirect file blocks.
  Journaling, extents, double/triple indirection and writes are rejected. }

const
  VFS_BLOCK_RPC_INFO = $4D424C4B00000001;
  VFS_BLOCK_RPC_READ = $4D424C4B00000002;
  EXT2_U_MAGIC = $EF53;
  EXT2_U_FEATURE_INCOMPAT_FILETYPE = $00000002;
  EXT2_U_FEATURE_COMPAT_HAS_JOURNAL = $00000004;
  EXT2_U_FEATURE_COMPAT_DIR_INDEX = $00000020;
  EXT2_U_FEATURE_RO_SUPPORTED = $00000003;
  EXT2_U_MODE_MASK = $F000;
  EXT2_U_MODE_REGULAR = $8000;
  EXT2_U_MODE_DIRECTORY = $4000;
  EXT2_U_MOUNT_PREFIX = '/mnt/disk';

type
  TExt2UserInode = record
    Mode: Word;
    Size: QWord;
    Blocks: array[0..14] of LongWord;
  end;

var
  BlockRequestEndpoint: QWord;
  BlockReplyEndpoint: QWord;
  Ext2Mounted: Boolean;
  Ext2DiskBytes: QWord;
  Ext2BaseBytes: QWord;
  Ext2PartitionBytes: QWord;
  Ext2BlocksCount: LongWord;
  Ext2InodesCount: LongWord;
  Ext2FirstDataBlock: LongWord;
  Ext2BlocksPerGroup: LongWord;
  Ext2InodesPerGroup: LongWord;
  Ext2BlockSize: LongWord;
  Ext2InodeSize: Word;
  Ext2GroupCount: LongWord;
  Ext2FeatureCompat: LongWord;
  Ext2FeatureIncompat: LongWord;
  Ext2FeatureRO: LongWord;
  Ext2SuperScratch: array[0..1023] of Byte;
  Ext2MetaScratch: array[0..255] of Byte;
  Ext2BlockScratch: array[0..4095] of Byte;
  Ext2IndirectScratch: array[0..4095] of Byte;
  Ext2SectorScratch: array[0..511] of Byte;
  { Keep completed file data separate from SharedBuffer, which blockd uses
    as its RPC transfer window for every metadata/data read. }
  Ext2FileScratch: array[0..65535] of Byte;
  Ext2DirEntries: array[0..127] of TMistixDirent;


function applyXORMask(x: number, mask: number): number {
  return nzb(x & mask) % 2;
}

const sliceBitsKaby = [
  [36, 35, 33, 32, 30, 28, 27, 26, 25, 24, 22, 20, 18, 17, 16, 14, 12, 10, 6],
  [37, 35, 34, 33, 31, 29, 28, 26, 24, 23, 22, 21, 20, 19, 17, 15, 13, 11, 7],
  [37, 36, 35, 34, 31, 30, 27, 26, 23, 22, 19, 16, 13, 12, 8],
];

function getSliceKaby(addr: number, masks: number[][]): number {
  assert(masks.length === 3);

  return (
    (applyXORMask(addr, masks[2]) << 2) |
    (applyXORMask(addr, masks[1]) << 1) |
    applyXORMask(addr, masks[0])
  );
}

const sliceBitsCoffee = [
  [6, 11, 12, 16, 18, 21, 22, 23, 24, 26, 30, 31],
  [7, 12, 13, 17, 19, 22, 23, 24, 25, 27, 31],
  [8, 13, 14, 18, 20, 23, 24, 25, 26, 28],
  [9, 14, 15, 19, 21, 24, 25, 26, 27, 29],
  [10, 15, 16, 20, 22, 25, 26, 27, 28, 30],
  [11, 16, 17, 21, 23, 26, 27, 28, 29, 31],
  [6, 8, 9, 10, 14, 15, 17, 18, 20, 23, 27, 30, 31],
];

function getSliceCoffee(addr: number, masks: number[][]): number {
  assert(masks.length === 7);

  const h0 = applyXORMask(addr, masks[6]);
  const h3 =
    (applyXORMask(addr, masks[2]) |
      applyXORMask(addr, masks[4]) |
      applyXORMask(addr, masks[5])) &
    (applyXORMask(addr, masks[2]) | applyXORMask(addr, masks[3])) &
    applyXORMask(addr, masks[0]);
  const h2 = ~h3 & applyXORMask(addr, masks[1]);
  return (h0 << 2) | (h2 << 1) | h3;
}

class LLCInfo {
  private readonly wayness: number;
  /* Cannot be derived from number of masks! */
  private readonly numSlices: number;
  private readonly numSameSliceOffsetsPerHugePage: number;
  private readonly numSetBits: number;
  private readonly sliceBitsHugePageMasks: number[][];
  private readonly sliceFunction: (addr: number, masks: number[][]) => number;

  constructor(
    wayness: number,
    numSlices: number,
    /*
     * Number of congruent (same slice!) addresses expected to be found for
     * each slice index when permutating the bits that are within the huge page
     * but above the set index bits. Turns out to be 4 for both Kaby and Coffee
     * Lake!
     */
    numSameSliceOffsetsPerHugePage: number,
    numSetBits: number,
    sliceBits: number[][],
    sliceFunction: (addr: number, masks: number[][]) => number,
  ) {
    this.wayness = wayness;
    this.numSlices = numSlices;
    this.numSameSliceOffsetsPerHugePage = numSameSliceOffsetsPerHugePage;
    this.numSetBits = numSetBits;
    this.sliceBitsHugePageMasks = this.sliceBitsHugePageMasks(sliceBits);
    this.sliceFunction = sliceFunction;
  }

  private sliceBitsHugePageMasks(sliceBits: number[][]): number[] {
    let masks: number[] = Array(sliceBits.length).fill(0);

    for (let i = 0; i < sliceBits.length; i++) {
      for (const bit of sliceBits[i]) {
        masks[i] = masks[i] ^ (bit <= 20 ? 1 << bit : 0);
      }
    }

    return masks;
  }

  /* Page offset to slice */
  public ptosl(p: number) {
    return this.sliceFunction(p, this.sliceBitsHugePageMasks);
  }

  get setMask() {
    let mask = (1 << this.numSetBits) - 1;
    return mask << NUM_LINE_BITS;
  }

  /* Offset to set */
  public ptose(p: number) {
    return (p & this.setMask) >> NUM_LINE_BITS;
  }

  /*
   * Minimum number of huge pages required for eviction set.  E.g., we need 5
   * huge pages for Kaby Lake because 5 huge pages * 4 numberOfOffsetsPerPage =
   * 20 > 16 = llcinfo.wayness
   */
  get numHugePagesPerEvictionSet() {
    let y = 1;
    while (y * this.numSameSliceOffsetsPerHugePage <= this.wayness) {
      y++;
    }
    return y;
  }
}

const UARCHS = { KABY: "Kaby Lake", COFFEE: "Coffee Lake" };

/** Microarchitecture of the system this code is running on */
const UARCH = UARCHS.KABY;

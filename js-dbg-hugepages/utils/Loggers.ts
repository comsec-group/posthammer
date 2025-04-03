function logError(error: Error, message: string = "") {
  console.log(`${error.name}` + (message ? `: ${message}` : ""));
  console.log(error.message);
  console.log(error.stack);
  throw error;
}

function logBitFlip(
  subFlipCount: number,
  totalFlipCount: number,
  allEvictionSetAddrs: Array<number>,
  dataPattIdx: number,
) {
  if (VERBOSE) {
    console.log(
      subFlipCount,
      totalFlipCount,
      allEvictionSetAddrs.length,
      "0x" + dataPatterns[dataPattIdx].toString(16),
    );
  } else {
    console.log(totalFlipCount, dataPatterns[dataPattIdx].toString(16));
  }
}

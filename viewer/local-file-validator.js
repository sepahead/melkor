const MAX_LOCAL_FILE_BYTES = 256 * 1024 * 1024;
const MAX_LOCAL_SPLATS = 5_000_000;
const MAX_PLY_HEADER_BYTES = 64 * 1024;
const MAX_SPZ_DECODED_BYTES = 512 * 1024 * 1024;
const MAX_SPZ_EXPANSION_RATIO = 100;
const MAX_ZIP_ENTRIES = 4096;
const MAX_ZIP_CENTRAL_BYTES = 16 * 1024 * 1024;
const MAX_ZIP_EXPANDED_BYTES = 512 * 1024 * 1024;
const MAX_ZIP_EXPANSION_RATIO = 100;
const MAX_SOG_METADATA_BYTES = 64 * 1024;
const MAX_SOG_IMAGE_DIMENSION = 16_384;
const MAX_SOG_IMAGE_PIXELS = MAX_LOCAL_SPLATS;
const MAX_SOG_RGBA_BYTES = 256 * 1024 * 1024;
const SOG_ENTRY_PREFIX_BYTES = 64;
const MAX_JSON_DEPTH = 64;
const FLOAT32_MAX = 3.4028234663852886e38;
const HALF_FLOAT_MAX = 65_504;
const MAX_SOG_LOG_POSITION = Math.log1p(HALF_FLOAT_MAX);
const SPARK_LOG_SCALE_MIN = -12;
const SPARK_LOG_SCALE_MAX = 9;
const SPARK_SCALE_MIN = Math.exp(SPARK_LOG_SCALE_MIN);
const SPARK_SCALE_MAX = Math.exp(SPARK_LOG_SCALE_MAX);

const CRC32_TABLE = new Uint32Array(256);
for (let value = 0; value < CRC32_TABLE.length; value++) {
  let crc = value;
  for (let bit = 0; bit < 8; bit++) {
    crc = (crc & 1) !== 0 ? 0xedb88320 ^ (crc >>> 1) : crc >>> 1;
  }
  CRC32_TABLE[value] = crc >>> 0;
}

const countFormatter = new Intl.NumberFormat("en-US", { maximumFractionDigits: 0 });
function formatCount(value) {
  return Number.isFinite(value) && value >= 0 ? countFormatter.format(value) : "—";
}

function formatBytes(bytes) {
  if (!Number.isFinite(bytes) || bytes < 0) return "—";
  if (bytes === 0) return "0 B";
  const units = ["B", "KiB", "MiB", "GiB"];
  const index = Math.min(Math.floor(Math.log(bytes) / Math.log(1024)), units.length - 1);
  const value = bytes / (1024 ** index);
  const digits = value >= 100 || index === 0 ? 0 : value >= 10 ? 1 : 2;
  return `${new Intl.NumberFormat("en-US", { maximumFractionDigits: digits }).format(value)} ${units[index]}`;
}

function localExtension(name) {
  const dot = name.lastIndexOf(".");
  return dot >= 0 ? name.slice(dot + 1).toLowerCase() : "";
}

function isRecord(value) {
  return value !== null && typeof value === "object" && !Array.isArray(value);
}

function isFiniteArray(value, length) {
  return Array.isArray(value) && value.length === length &&
    value.every((item) => typeof item === "number" && Number.isFinite(item));
}

function parseStrictJson(text) {
  let offset = 0;

  const fail = () => {
    throw new Error("invalid strict JSON");
  };
  const skipWhitespace = () => {
    while (offset < text.length && /[\u0009\u000a\u000d\u0020]/.test(text[offset])) offset++;
  };
  const parseString = () => {
    const start = offset++;
    while (offset < text.length) {
      const code = text.charCodeAt(offset);
      if (code === 0x22) {
        offset++;
        return JSON.parse(text.slice(start, offset));
      }
      if (code < 0x20) fail();
      if (code === 0x5c) {
        offset++;
        if (offset >= text.length) fail();
        const escape = text[offset];
        if (escape === "u") {
          if (!/^[0-9A-Fa-f]{4}$/.test(text.slice(offset + 1, offset + 5))) fail();
          offset += 5;
          continue;
        }
        if (!'"\\/bfnrt'.includes(escape)) fail();
      }
      offset++;
    }
    fail();
  };
  const parseValue = (depth) => {
    if (depth > MAX_JSON_DEPTH) fail();
    skipWhitespace();
    const token = text[offset];
    if (token === '"') return parseString();
    if (token === "{") {
      offset++;
      skipWhitespace();
      const result = Object.create(null);
      if (text[offset] === "}") {
        offset++;
        return result;
      }
      while (offset < text.length) {
        if (text[offset] !== '"') fail();
        const key = parseString();
        if (Object.hasOwn(result, key)) fail();
        skipWhitespace();
        if (text[offset++] !== ":") fail();
        result[key] = parseValue(depth + 1);
        skipWhitespace();
        const separator = text[offset++];
        if (separator === "}") return result;
        if (separator !== ",") fail();
        skipWhitespace();
      }
      fail();
    }
    if (token === "[") {
      offset++;
      skipWhitespace();
      const result = [];
      if (text[offset] === "]") {
        offset++;
        return result;
      }
      while (offset < text.length) {
        result.push(parseValue(depth + 1));
        skipWhitespace();
        const separator = text[offset++];
        if (separator === "]") return result;
        if (separator !== ",") fail();
        skipWhitespace();
      }
      fail();
    }
    for (const [literal, value] of [["true", true], ["false", false], ["null", null]]) {
      if (text.startsWith(literal, offset)) {
        offset += literal.length;
        return value;
      }
    }
    const match = /^-?(?:0|[1-9][0-9]*)(?:\.[0-9]+)?(?:[eE][+-]?[0-9]+)?/.exec(
      text.slice(offset),
    );
    if (!match) fail();
    offset += match[0].length;
    const value = Number(match[0]);
    if (!Number.isFinite(value)) fail();
    return value;
  };

  const result = parseValue(0);
  skipWhitespace();
  if (offset !== text.length) fail();
  return result;
}

function isOrderedFiniteRange(mins, maxs, length) {
  return isFiniteArray(mins, length) && isFiniteArray(maxs, length) &&
    mins.every((minimum, index) => minimum <= maxs[index] &&
      Number.isFinite(maxs[index] - minimum));
}

function valuesFitFloat32(values) {
  return values.every((value) => Math.abs(value) <= FLOAT32_MAX);
}

function logScalesFitSpark(values) {
  return values.every((value) => Number.isFinite(value) &&
    value >= SPARK_LOG_SCALE_MIN && value <= SPARK_LOG_SCALE_MAX);
}

function scalesFitSpark(values) {
  return values.every((value) => value === 0 ||
    (Number.isFinite(value) && value >= SPARK_SCALE_MIN && value <= SPARK_SCALE_MAX));
}

function halfToNumber(bits) {
  const sign = (bits & 0x8000) === 0 ? 1 : -1;
  const exponent = (bits >>> 10) & 0x1f;
  const fraction = bits & 0x03ff;
  if (exponent === 0) return sign * fraction * (2 ** -24);
  if (exponent === 0x1f) return fraction === 0 ? sign * Infinity : Number.NaN;
  return sign * (1 + fraction / 1024) * (2 ** (exponent - 15));
}

function throwIfCanceled(shouldCancel) {
  if (!shouldCancel()) return;
  const error = new Error("The local file check was canceled.");
  error.name = "AbortError";
  throw error;
}

async function yieldForCancellation(shouldCancel) {
  await new Promise((resolve) => setTimeout(resolve, 0));
  throwIfCanceled(shouldCancel);
}

function isSafeSogReference(value) {
  return typeof value === "string" && value.length > 0 && value.length <= 255 &&
    !value.includes("\\") && !value.startsWith("/") && !value.includes("//") &&
    value.split("/").every((part) => part !== "" && part !== "." && part !== "..") &&
    /\.(png|webp)$/i.test(value);
}

function bytesMatch(bytes, offset, expected) {
  return expected.every((value, index) => bytes[offset + index] === value);
}

function asciiAt(bytes, offset, text) {
  return [...text].every((value, index) => bytes[offset + index] === value.charCodeAt(0));
}

function readUint24LittleEndian(bytes, offset) {
  return bytes[offset] | (bytes[offset + 1] << 8) | (bytes[offset + 2] << 16);
}

function readSogImageDimensions(entry) {
  const bytes = entry.prefix;
  const view = new DataView(bytes.buffer, bytes.byteOffset, bytes.byteLength);
  const extension = localExtension(entry.name);
  let width = 0;
  let height = 0;
  if (extension === "png") {
    const signature = [0x89, 0x50, 0x4e, 0x47, 0x0d, 0x0a, 0x1a, 0x0a];
    if (bytes.length < 24 || !bytesMatch(bytes, 0, signature) ||
        view.getUint32(8, false) !== 13 || !asciiAt(bytes, 12, "IHDR")) {
      throw new Error(`SOG image ${entry.name} has an invalid PNG header.`);
    }
    width = view.getUint32(16, false);
    height = view.getUint32(20, false);
  } else if (extension === "webp") {
    if (bytes.length < 30 || !asciiAt(bytes, 0, "RIFF") || !asciiAt(bytes, 8, "WEBP") ||
        view.getUint32(4, true) + 8 !== entry.expandedBytes) {
      throw new Error(`SOG image ${entry.name} has an invalid WebP header.`);
    }
    const chunk = String.fromCharCode(...bytes.subarray(12, 16));
    if (chunk === "VP8X") {
      if (view.getUint32(16, true) < 10 || (bytes[20] & 0x02) !== 0) {
        throw new Error(`SOG image ${entry.name} uses an unsupported animated WebP.`);
      }
      width = 1 + readUint24LittleEndian(bytes, 24);
      height = 1 + readUint24LittleEndian(bytes, 27);
    } else if (chunk === "VP8L") {
      if (bytes[20] !== 0x2f) {
        throw new Error(`SOG image ${entry.name} has an invalid VP8L header.`);
      }
      const packed = view.getUint32(21, true);
      width = 1 + (packed & 0x3fff);
      height = 1 + ((packed >>> 14) & 0x3fff);
    } else if (chunk === "VP8 ") {
      if (!bytesMatch(bytes, 23, [0x9d, 0x01, 0x2a])) {
        throw new Error(`SOG image ${entry.name} has an invalid WebP frame header.`);
      }
      width = view.getUint16(26, true) & 0x3fff;
      height = view.getUint16(28, true) & 0x3fff;
    } else {
      throw new Error(`SOG image ${entry.name} uses an unsupported WebP layout.`);
    }
  } else {
    throw new Error(`SOG image ${entry.name} must use PNG or WebP.`);
  }
  const pixels = width * height;
  if (width < 1 || height < 1 || width > MAX_SOG_IMAGE_DIMENSION ||
      height > MAX_SOG_IMAGE_DIMENSION || !Number.isSafeInteger(pixels) ||
      pixels > MAX_SOG_IMAGE_PIXELS) {
    throw new Error(`SOG image ${entry.name} exceeds the local pixel limit.`);
  }
  return { height, pixels, width };
}

function validateSogMetadata(entries) {
  const metadataEntries = entries.filter((entry) => entry.name.split("/").pop() === "meta.json");
  if (metadataEntries.length !== 1) {
    throw new Error("A SOG archive must contain exactly one meta.json file.");
  }
  const metadataEntry = metadataEntries[0];
  if (metadataEntry.expandedBytes > MAX_SOG_METADATA_BYTES ||
      metadataEntry.prefix.length !== metadataEntry.expandedBytes) {
    throw new Error(`SOG metadata is limited to ${formatBytes(MAX_SOG_METADATA_BYTES)}.`);
  }
  let text;
  try {
    text = new TextDecoder("utf-8", { fatal: true }).decode(metadataEntry.prefix);
  } catch {
    throw new Error("The SOG meta.json file is not valid UTF-8.");
  }
  let metadata;
  try {
    metadata = parseStrictJson(text);
  } catch {
    throw new Error("The SOG meta.json file is not valid strict JSON.");
  }
  if (!isRecord(metadata)) throw new Error("The SOG metadata root must be an object.");
  const isVersion2 = Object.hasOwn(metadata, "version");
  if (isVersion2 && metadata.version !== 2) {
    throw new Error("The SOG metadata version is not supported.");
  }
  for (const key of ["means", "scales", "quats", "sh0"]) {
    if (!isRecord(metadata[key])) throw new Error(`The SOG ${key} metadata is missing.`);
  }
  const count = isVersion2 ? metadata.count : metadata.means.shape?.[0];
  if (!Number.isSafeInteger(count) || count < 1 || count > MAX_LOCAL_SPLATS) {
    throw new Error(`Local scenes are limited to ${formatCount(MAX_LOCAL_SPLATS)} splats.`);
  }

  const requireFiles = (field, expectedCount) => {
    const files = metadata[field]?.files;
    if (!Array.isArray(files) || files.length !== expectedCount ||
        !files.every(isSafeSogReference) || new Set(files).size !== files.length) {
      throw new Error(`The SOG ${field} file list is invalid.`);
    }
    return files;
  };
  const files = {
    means: requireFiles("means", 2),
    scales: requireFiles("scales", 1),
    quats: requireFiles("quats", 1),
    sh0: requireFiles("sh0", 1),
    shN: metadata.shN === undefined ? [] : requireFiles("shN", 2),
  };
  if (metadata.shN !== undefined && !isRecord(metadata.shN)) {
    throw new Error("The SOG shN metadata is invalid.");
  }

  if (!isOrderedFiniteRange(metadata.means.mins, metadata.means.maxs, 3) ||
      ![...metadata.means.mins, ...metadata.means.maxs].every(
        (value) => Math.abs(value) <= MAX_SOG_LOG_POSITION,
      )) {
    throw new Error("The SOG means range is invalid.");
  }
  if (isVersion2) {
    if (!isFiniteArray(metadata.scales.codebook, 256) ||
        !isFiniteArray(metadata.sh0.codebook, 256) ||
        !logScalesFitSpark(metadata.scales.codebook) ||
        !valuesFitFloat32(metadata.sh0.codebook)) {
      throw new Error("The SOG v2 codebook is invalid.");
    }
    if (metadata.quats.encoding !== undefined &&
        metadata.quats.encoding !== "quaternion_packed") {
      throw new Error("The SOG quaternion encoding is not supported.");
    }
    if (metadata.shN !== undefined) {
      if (!Number.isInteger(metadata.shN.bands) || metadata.shN.bands < 1 ||
          metadata.shN.bands > 3) {
        throw new Error("The SOG v2 SH band count is invalid.");
      }
      const validLookup = (isFiniteArray(metadata.shN.codebook, 256) &&
          valuesFitFloat32(metadata.shN.codebook)) ||
        (typeof metadata.shN.mins === "number" && Number.isFinite(metadata.shN.mins) &&
          typeof metadata.shN.maxs === "number" && Number.isFinite(metadata.shN.maxs) &&
          metadata.shN.mins <= metadata.shN.maxs &&
          Number.isFinite(metadata.shN.maxs - metadata.shN.mins) &&
          valuesFitFloat32([metadata.shN.mins, metadata.shN.maxs]));
      if (!validLookup) throw new Error("The SOG v2 SH lookup is invalid.");
    }
  } else {
    const shapeMatches = (value, expected) => Array.isArray(value) &&
      value.length === expected.length && value.every((item, index) => item === expected[index]);
    if (!shapeMatches(metadata.means.shape, [count, 3]) ||
        !shapeMatches(metadata.scales.shape, [count, 3]) ||
        !shapeMatches(metadata.quats.shape, [count, 4]) ||
        !shapeMatches(metadata.sh0.shape, [count, 1, 4]) ||
        metadata.quats.encoding !== "quaternion_packed" ||
        !isOrderedFiniteRange(metadata.scales.mins, metadata.scales.maxs, 3) ||
        !logScalesFitSpark([...metadata.scales.mins, ...metadata.scales.maxs]) ||
        !isOrderedFiniteRange(metadata.sh0.mins, metadata.sh0.maxs, 4) ||
        !valuesFitFloat32([...metadata.sh0.mins, ...metadata.sh0.maxs])) {
      throw new Error("The SOG v1 field layout is invalid.");
    }
    if (metadata.shN !== undefined) {
      const coefficientCount = metadata.shN.shape?.[1];
      if (!shapeMatches(metadata.shN.shape, [count, coefficientCount]) ||
          ![9, 24, 45].includes(coefficientCount) ||
          typeof metadata.shN.mins !== "number" || !Number.isFinite(metadata.shN.mins) ||
          typeof metadata.shN.maxs !== "number" || !Number.isFinite(metadata.shN.maxs) ||
          metadata.shN.mins > metadata.shN.maxs ||
          !Number.isFinite(metadata.shN.maxs - metadata.shN.mins) ||
          !valuesFitFloat32([metadata.shN.mins, metadata.shN.maxs])) {
        throw new Error("The SOG v1 SH layout is invalid.");
      }
    }
  }

  const metadataSlash = metadataEntry.name.lastIndexOf("/");
  const prefix = metadataSlash < 0 ? "" : metadataEntry.name.slice(0, metadataSlash + 1);
  const byName = new Map(entries.map((entry) => [entry.name, entry]));
  const imageEntries = [];
  for (const reference of Object.values(files).flat()) {
    const entry = byName.get(prefix + reference);
    if (!entry || entry.name.endsWith("/")) {
      throw new Error(`The SOG image ${reference} is missing.`);
    }
    imageEntries.push(entry);
  }
  let rgbaBytes = 0;
  const mainImageCount = files.means.length + files.scales.length +
    files.quats.length + files.sh0.length;
  for (let index = 0; index < imageEntries.length; index++) {
    const dimensions = readSogImageDimensions(imageEntries[index]);
    const isMainImage = index < mainImageCount ||
      (files.shN.length === 2 && index === mainImageCount + 1);
    if (isMainImage && dimensions.pixels < count) {
      throw new Error(`SOG image ${imageEntries[index].name} is smaller than the splat count.`);
    }
    if (files.shN.length === 2 && index === mainImageCount &&
        (dimensions.width < 960 || dimensions.height < 1024)) {
      throw new Error("The SOG SH centroid image must be at least 960 by 1024 pixels.");
    }
    rgbaBytes += dimensions.pixels * 4;
    if (!Number.isSafeInteger(rgbaBytes) || rgbaBytes > MAX_SOG_RGBA_BYTES) {
      throw new Error(`Decoded SOG images are limited to ${formatBytes(MAX_SOG_RGBA_BYTES)}.`);
    }
  }
}

function readPlyScalar(view, offset, type, littleEndian) {
  switch (type) {
    case "char": return view.getInt8(offset);
    case "uchar": return view.getUint8(offset);
    case "short": return view.getInt16(offset, littleEndian);
    case "ushort": return view.getUint16(offset, littleEndian);
    case "int": return view.getInt32(offset, littleEndian);
    case "uint": return view.getUint32(offset, littleEndian);
    case "float": return view.getFloat32(offset, littleEndian);
    case "double": return view.getFloat64(offset, littleEndian);
    default: throw new Error("That PLY file uses an unsupported scalar type.");
  }
}

function readPlyProperty(view, recordOffset, property, littleEndian) {
  return readPlyScalar(view, recordOffset + property.offset, property.type, littleEndian);
}

async function visitPlyRecords(file, headerBytes, element, shouldCancel, callback) {
  const targetBatchBytes = 4 * 1024 * 1024;
  const recordsPerBatch = Math.max(1, Math.floor(targetBatchBytes / element.stride));
  for (let first = 0; first < element.count; first += recordsPerBatch) {
    throwIfCanceled(shouldCancel);
    const count = Math.min(recordsPerBatch, element.count - first);
    const start = headerBytes + element.dataOffset + first * element.stride;
    const bytes = new Uint8Array(
      await file.slice(start, start + count * element.stride).arrayBuffer(),
    );
    if (bytes.byteLength !== count * element.stride) {
      throw new Error("That PLY file changed during validation.");
    }
    const view = new DataView(bytes.buffer, bytes.byteOffset, bytes.byteLength);
    for (let index = 0; index < count; index++) {
      callback(view, index * element.stride, first + index);
    }
    await yieldForCancellation(shouldCancel);
  }
}

async function validatePlyContainer(file, shouldCancel) {
  const prefix = new Uint8Array(
    await file.slice(0, Math.min(file.size, MAX_PLY_HEADER_BYTES + 1)).arrayBuffer(),
  );
  const text = new TextDecoder("latin1").decode(prefix);
  const terminator = "\nend_header\n";
  const terminatorOffset = text.indexOf(terminator);
  if (terminatorOffset < 0) {
    if (prefix.byteLength > MAX_PLY_HEADER_BYTES) {
      throw new Error(`PLY headers are limited to ${formatBytes(MAX_PLY_HEADER_BYTES)}.`);
    }
    throw new Error("That PLY file has no complete header.");
  }
  const headerBytes = terminatorOffset + terminator.length;
  if (headerBytes > MAX_PLY_HEADER_BYTES ||
      prefix.subarray(0, headerBytes).some((byte) => byte > 0x7f || byte === 0)) {
    throw new Error("That PLY file has an invalid header.");
  }
  const header = text.slice(0, headerBytes);
  if (header.includes("\r")) {
    throw new Error("That PLY file uses line endings that the viewer cannot decode.");
  }
  const lines = header.trimEnd().split("\n");
  if (lines[0] !== "ply") throw new Error("That PLY file has an invalid signature.");
  const formats = lines.filter((line) => line.startsWith("format "));
  if (formats.length !== 1 ||
      !/^format binary_(little|big)_endian 1\.0$/.test(formats[0])) {
    throw new Error("That PLY file has an unsupported format declaration.");
  }

  const profileMarkers = lines
    .map((line) => /^comment melkor_profile ([^\s]+)$/.exec(line))
    .filter(Boolean)
    .map((match) => match[1]);
  if (profileMarkers.length > 1) {
    throw new Error("That PLY file has duplicate Melkor profile markers.");
  }
  const profileMarker = profileMarkers[0] ?? null;
  if (profileMarker === "melkor-canonical-v1" ||
      profileMarker === "ply:melkor-canonical-v1") {
    throw new Error(
      "The viewer does not support Melkor canonical PLY. Convert it to ply:graphdeco-3dgs-v1.",
    );
  }
  if (profileMarker !== null && profileMarker !== "graphdeco-3dgs-v1" &&
      profileMarker !== "ply:graphdeco-3dgs-v1") {
    throw new Error("That PLY file uses an unsupported Melkor profile marker.");
  }

  const typeBytes = new Map([
    ["char", 1], ["uchar", 1], ["short", 2], ["ushort", 2],
    ["int", 4], ["uint", 4], ["float", 4], ["double", 8],
  ]);
  const elements = [];
  const elementNames = new Set();
  let currentElement = null;
  let formatSeen = false;
  for (let index = 1; index < lines.length; index++) {
    const line = lines[index];
    if (line === "end_header") break;
    if (line.startsWith("comment ") || line.startsWith("obj_info ")) continue;
    if (line.startsWith("format ")) {
      if (formatSeen || elements.length > 0 || line !== formats[0]) {
        throw new Error("That PLY file has an invalid format declaration.");
      }
      formatSeen = true;
      continue;
    }
    const elementMatch = /^element ([A-Za-z0-9_]+) (0|[1-9][0-9]*)$/.exec(line);
    if (elementMatch) {
      const count = Number(elementMatch[2]);
      if (!Number.isSafeInteger(count) || elementNames.has(elementMatch[1])) {
        throw new Error("That PLY file has invalid element metadata.");
      }
      if (count > MAX_LOCAL_SPLATS) {
        if (elementMatch[1] === "vertex") {
          throw new Error(`Local scenes are limited to ${formatCount(MAX_LOCAL_SPLATS)} splats.`);
        }
        throw new Error("That PLY file has too many non-splat records.");
      }
      currentElement = {
        count,
        dataOffset: 0,
        name: elementMatch[1],
        names: new Set(),
        properties: [],
        stride: 0,
      };
      elements.push(currentElement);
      elementNames.add(currentElement.name);
      continue;
    }
    if (line.startsWith("property list ")) {
      throw new Error("PLY list properties are not supported for local splats.");
    }
    const propertyMatch = /^property (char|uchar|short|ushort|int|uint|float|double) ([A-Za-z0-9_]+)$/.exec(line);
    if (propertyMatch) {
      if (!currentElement || currentElement.names.has(propertyMatch[2])) {
        throw new Error("That PLY file has invalid property metadata.");
      }
      currentElement.properties.push({
        name: propertyMatch[2],
        offset: currentElement.stride,
        type: propertyMatch[1],
      });
      currentElement.stride += typeBytes.get(propertyMatch[1]);
      currentElement.names.add(propertyMatch[2]);
      continue;
    }
    throw new Error("That PLY file has an unsupported header directive.");
  }
  if (!formatSeen || elements.some((element) => element.properties.length === 0)) {
    throw new Error("That PLY file has incomplete element metadata.");
  }
  let dataBytes = 0;
  for (const element of elements) {
    element.dataOffset = dataBytes;
    dataBytes += element.stride * element.count;
  }
  if (!Number.isSafeInteger(dataBytes) || file.size - headerBytes !== dataBytes) {
    throw new Error("That PLY file size does not match its declared elements.");
  }
  const littleEndian = formats[0].includes("little_endian");

  const vertex = elements.find((element) => element.name === "vertex");
  if (!vertex) throw new Error("That PLY file has no vertex element.");
  const count = vertex.count;
  if (!Number.isSafeInteger(count) || count < 1) {
    throw new Error("That PLY file has no splats.");
  }
  if (count > MAX_LOCAL_SPLATS) {
    throw new Error(`Local scenes are limited to ${formatCount(MAX_LOCAL_SPLATS)} splats.`);
  }
  const names = vertex.names;
  const propertiesByName = new Map(vertex.properties.map((property) => [property.name, property]));
  const hasAll = (required) => required.every((name) => names.has(name));
  const hasAny = (required) => required.some((name) => names.has(name));
  const scaleNames = ["scale_0", "scale_1", "scale_2"];
  const rotationNames = ["rot_0", "rot_1", "rot_2", "rot_3"];
  const dcNames = ["f_dc_0", "f_dc_1", "f_dc_2"];
  const rgbNames = ["red", "green", "blue"];
  const canonicalNames = [
    "scale_x", "scale_y", "scale_z",
    "rotation_x", "rotation_y", "rotation_z", "rotation_w",
    "sh_0_r", "sh_0_g", "sh_0_b",
  ];
  if (hasAny(canonicalNames)) {
    throw new Error(
      "The viewer does not support Melkor canonical PLY. Convert it to ply:graphdeco-3dgs-v1.",
    );
  }
  if ((hasAny(scaleNames) && !hasAll(scaleNames)) ||
      (hasAny(rotationNames) && !hasAll(rotationNames)) ||
      (hasAny(dcNames) && !hasAll(dcNames)) ||
      (hasAny(rgbNames) && !hasAll(rgbNames))) {
    throw new Error("That PLY file has an incomplete splat property group.");
  }
  if (hasAll(dcNames) && hasAll(rgbNames)) {
    throw new Error("That PLY file has multiple color property groups.");
  }
  if (names.has("opacity") && names.has("alpha")) {
    throw new Error("That PLY file has multiple opacity properties.");
  }
  if (profileMarker !== null &&
      (!hasAll([...scaleNames, ...rotationNames, ...dcNames, "opacity"]) ||
       !["x", "y", "z", ...scaleNames, ...rotationNames, ...dcNames, "opacity"]
         .every((name) => propertiesByName.get(name)?.type === "float") ||
       vertex.properties.some(({ name, type }) =>
         /^f_rest_[0-9]+$/.test(name) && type !== "float"))) {
    throw new Error("That PLY file does not match its Graphdeco profile marker.");
  }

  const shIndices = (element) => element.properties
    .map(({ name }) => /^f_rest_([0-9]+)$/.exec(name))
    .filter(Boolean)
    .map((match) => Number(match[1]))
    .sort((left, right) => left - right);
  const validateShLayout = (element, allowedCounts) => {
    const indices = shIndices(element);
    if (!allowedCounts.includes(indices.length) ||
        indices.some((value, index) => value !== index)) {
      throw new Error("That PLY file has an unsupported SH property layout.");
    }
    return indices;
  };

  const chunk = elements.find((element) => element.name === "chunk");
  if (chunk) {
    const sh = elements.find((element) => element.name === "sh");
    const expectedOrder = sh ? ["chunk", "vertex", "sh"] : ["chunk", "vertex"];
    const chunkProperties = new Map(chunk.properties.map((property) => [property.name, property]));
    const rangeNames = [
      "min_x", "min_y", "min_z", "max_x", "max_y", "max_z",
      "min_scale_x", "min_scale_y", "min_scale_z",
      "max_scale_x", "max_scale_y", "max_scale_z",
    ];
    const colorRangeNames = ["min_r", "min_g", "min_b", "max_r", "max_g", "max_b"];
    const packedNames = ["packed_position", "packed_rotation", "packed_scale", "packed_color"];
    if (elements.map((element) => element.name).join(",") !== expectedOrder.join(",") ||
        chunk.count !== Math.ceil(count / 256) || (sh && sh.count !== count) ||
        !hasAll(packedNames) || !rangeNames.every((name) => chunk.names.has(name)) ||
        packedNames.some((name) => propertiesByName.get(name)?.type !== "uint") ||
        rangeNames.some((name) => chunkProperties.get(name)?.type !== "float") ||
        (colorRangeNames.some((name) => chunk.names.has(name)) &&
         !colorRangeNames.every((name) => chunkProperties.get(name)?.type === "float"))) {
      throw new Error("That PLY file has invalid compressed splat elements.");
    }
    validateShLayout(vertex, [0]);
    if (sh) {
      validateShLayout(sh, [9, 24, 45]);
      if (sh.properties.some((property) => property.type !== "uchar")) {
        throw new Error("That PLY file has invalid compressed SH properties.");
      }
    }

    await visitPlyRecords(file, headerBytes, chunk, shouldCancel, (view, base, index) => {
      const value = (name) => {
        const property = chunkProperties.get(name);
        return readPlyProperty(view, base, property, littleEndian);
      };
      for (const axis of ["x", "y", "z"]) {
        const minimum = value(`min_${axis}`);
        const maximum = value(`max_${axis}`);
        if (!Number.isFinite(minimum) || !Number.isFinite(maximum) || minimum > maximum ||
            Math.abs(minimum) > HALF_FLOAT_MAX || Math.abs(maximum) > HALF_FLOAT_MAX) {
          throw new Error(`PLY chunk ${formatCount(index)} has an invalid position range.`);
        }
        const minimumScale = value(`min_scale_${axis}`);
        const maximumScale = value(`max_scale_${axis}`);
        if (!Number.isFinite(minimumScale) || !Number.isFinite(maximumScale) ||
            minimumScale > maximumScale ||
            !logScalesFitSpark([minimumScale, maximumScale])) {
          throw new Error(`PLY chunk ${formatCount(index)} has an invalid scale range.`);
        }
      }
      if (colorRangeNames.every((name) => chunk.names.has(name))) {
        for (const channel of ["r", "g", "b"]) {
          const minimum = value(`min_${channel}`);
          const maximum = value(`max_${channel}`);
          if (!Number.isFinite(minimum) || !Number.isFinite(maximum) || minimum > maximum) {
            throw new Error(`PLY chunk ${formatCount(index)} has an invalid color range.`);
          }
        }
      }
    });
    const packedRotation = propertiesByName.get("packed_rotation");
    await visitPlyRecords(file, headerBytes, vertex, shouldCancel, (view, base, index) => {
      const bits = readPlyProperty(view, base, packedRotation, littleEndian) >>> 0;
      const components = [
        (bits >>> 20) & 1023,
        (bits >>> 10) & 1023,
        bits & 1023,
      ].map((value) => (value / 1023 - 0.5) * Math.SQRT2);
      const squaredNorm = components.reduce((sum, value) => sum + value * value, 0);
      const quantizationError = Math.SQRT2 / (2 * 1023);
      const maximumSquaredNorm = 1 + 2 * Math.sqrt(3) * quantizationError +
        3 * quantizationError * quantizationError;
      if (squaredNorm > maximumSquaredNorm) {
        throw new Error(`PLY splat ${formatCount(index)} has an invalid packed rotation.`);
      }
    });
  } else if (elements.length !== 1 || !hasAll(["x", "y", "z"])) {
    throw new Error("That PLY file has an unsupported element layout.");
  } else {
    validateShLayout(vertex, [0, 9, 24, 45]);
  }

  if (!chunk) {
    const usedNames = new Set([
      "x", "y", "z", "opacity", "alpha",
      ...scaleNames, ...rotationNames, ...dcNames, ...rgbNames,
      ...vertex.properties.map(({ name }) => /^f_rest_[0-9]+$/.test(name) ? name : null),
    ].filter(Boolean));
    const usedProperties = vertex.properties.filter(({ name }) => usedNames.has(name));
    const positionProperties = ["x", "y", "z"].map((name) => propertiesByName.get(name));
    const scaleProperties = scaleNames.map((name) => propertiesByName.get(name));
    const rotationProperties = rotationNames.map((name) => propertiesByName.get(name));
    await visitPlyRecords(file, headerBytes, vertex, shouldCancel, (view, base, index) => {
      for (const property of usedProperties) {
        const value = readPlyProperty(view, base, property, littleEndian);
        if (!Number.isFinite(value)) {
          throw new Error(`PLY splat ${formatCount(index)} has a non-finite ${property.name} value.`);
        }
        if (Math.abs(value) > FLOAT32_MAX) {
          throw new Error(
            `PLY splat ${formatCount(index)} has a ${property.name} value outside the viewer range.`,
          );
        }
      }
      if (positionProperties.some((property) =>
        Math.abs(readPlyProperty(view, base, property, littleEndian)) > HALF_FLOAT_MAX)) {
        throw new Error(`PLY splat ${formatCount(index)} exceeds the viewer position range.`);
      }
      if (hasAll(scaleNames) && scaleProperties.some((property) => {
        const scale = readPlyProperty(view, base, property, littleEndian);
        return !logScalesFitSpark([scale]);
      })) {
        throw new Error(`PLY splat ${formatCount(index)} has an invalid scale.`);
      }
      if (hasAll(rotationNames)) {
        const squaredNorm = rotationProperties.reduce(
          (sum, property) => {
            const value = readPlyProperty(view, base, property, littleEndian);
            return sum + value * value;
          },
          0,
        );
        if (!Number.isFinite(squaredNorm) || squaredNorm <= 1e-12) {
          throw new Error(`PLY splat ${formatCount(index)} has an invalid rotation.`);
        }
      }
    });
  }
}

async function readSpzGzip(file, shouldCancel) {
  if (typeof DecompressionStream !== "function") {
    throw new Error("This browser cannot decode SPZ files.");
  }
  const reader = file.stream().pipeThrough(new DecompressionStream("gzip")).getReader();
  const prefix = new Uint8Array(16);
  let offset = 0;
  let decodedBytes = 0;
  let plan = null;
  const positionRecord = new Uint8Array(3);
  let positionRecordBytes = 0;
  let positionIndex = 0;
  const rotationRecord = new Uint8Array(4);
  let rotationRecordBytes = 0;
  let rotationIndex = 0;
  const lodCountRecord = new Uint8Array(2);
  let lodCountRecordBytes = 0;
  let lodCountIndex = 0;
  let lodCounts = null;
  const lodStartRecord = new Uint8Array(4);
  let lodStartRecordBytes = 0;
  let lodStartIndex = 0;
  try {
    while (true) {
      let chunk;
      try {
        chunk = await reader.read();
      } catch {
        throw new Error("That SPZ file has an invalid gzip stream.");
      }
      const { done, value } = chunk;
      if (done) break;
      throwIfCanceled(shouldCancel);
      const chunkOffset = decodedBytes;
      decodedBytes += value.byteLength;
      if (offset < prefix.byteLength) {
        const copied = Math.min(value.byteLength, prefix.byteLength - offset);
        prefix.set(value.subarray(0, copied), offset);
        offset += copied;
      }
      if (offset === prefix.byteLength && plan === null) {
        plan = validateSpzHeader(prefix);
        if (plan.decodedBytes > file.size * MAX_SPZ_EXPANSION_RATIO) {
          throw new Error(`The SPZ expansion ratio exceeds ${MAX_SPZ_EXPANSION_RATIO}:1.`);
        }
        if (plan.hasLod) lodCounts = new Uint16Array(plan.count);
      }
      if (plan !== null) {
        const positionOverlapStart = Math.max(plan.positionOffset, chunkOffset);
        const positionOverlapEnd = Math.min(plan.positionEnd, decodedBytes);
        for (let position = positionOverlapStart; position < positionOverlapEnd; position++) {
          positionRecord[positionRecordBytes++] = value[position - chunkOffset];
          if (positionRecordBytes === plan.positionScalarBytes) {
            validateSpzPosition(positionRecord, plan, positionIndex++);
            positionRecordBytes = 0;
          }
        }
        const overlapStart = Math.max(plan.rotationOffset, chunkOffset);
        const overlapEnd = Math.min(plan.rotationEnd, decodedBytes);
        for (let position = overlapStart; position < overlapEnd; position++) {
          rotationRecord[rotationRecordBytes++] = value[position - chunkOffset];
          if (rotationRecordBytes === plan.rotationBytes) {
            validateSpzRotation(rotationRecord, plan.version, rotationIndex++);
            rotationRecordBytes = 0;
          }
        }
        if (plan.hasLod) {
          const countOverlapStart = Math.max(plan.lodCountOffset, chunkOffset);
          const countOverlapEnd = Math.min(plan.lodCountEnd, decodedBytes);
          for (let position = countOverlapStart; position < countOverlapEnd; position++) {
            lodCountRecord[lodCountRecordBytes++] = value[position - chunkOffset];
            if (lodCountRecordBytes === lodCountRecord.length) {
              lodCounts[lodCountIndex++] = lodCountRecord[0] | (lodCountRecord[1] << 8);
              lodCountRecordBytes = 0;
            }
          }
          const startOverlapStart = Math.max(plan.lodStartOffset, chunkOffset);
          const startOverlapEnd = Math.min(plan.lodStartEnd, decodedBytes);
          for (let position = startOverlapStart; position < startOverlapEnd; position++) {
            lodStartRecord[lodStartRecordBytes++] = value[position - chunkOffset];
            if (lodStartRecordBytes === lodStartRecord.length) {
              const start = (lodStartRecord[0] | (lodStartRecord[1] << 8) |
                (lodStartRecord[2] << 16) | (lodStartRecord[3] << 24)) >>> 0;
              const childCount = lodCounts[lodStartIndex];
              if (childCount > 0 &&
                  (start >= plan.count || childCount > plan.count - start)) {
                throw new Error(
                  `SPZ LOD node ${formatCount(lodStartIndex)} has an invalid child range.`,
                );
              }
              lodStartIndex++;
              lodStartRecordBytes = 0;
            }
          }
        }
      }
      if (plan !== null && decodedBytes > plan.decodedBytes) {
        throw new Error("That SPZ file contains data beyond its declared splats.");
      }
    }
  } finally {
    try { await reader.cancel(); } catch {}
  }
  if (offset !== prefix.byteLength) throw new Error("That SPZ file has a truncated header.");
  if (decodedBytes !== plan.decodedBytes) {
    throw new Error("That SPZ file is shorter than its declared splats.");
  }
  if (positionRecordBytes !== 0 || positionIndex !== plan.count * 3) {
    throw new Error("That SPZ file has incomplete position data.");
  }
  if (rotationRecordBytes !== 0 || rotationIndex !== plan.count) {
    throw new Error("That SPZ file has incomplete rotation data.");
  }
  if (plan.hasLod && (lodCountRecordBytes !== 0 || lodCountIndex !== plan.count ||
      lodStartRecordBytes !== 0 || lodStartIndex !== plan.count)) {
    throw new Error("That SPZ file has incomplete LOD data.");
  }
}

function validateSpzPosition(encoded, plan, scalarIndex) {
  if (plan.version === 1) {
    const bits = encoded[0] | (encoded[1] << 8);
    if ((bits & 0x7c00) === 0x7c00) {
      throw new Error(
        `SPZ position ${formatCount(Math.floor(scalarIndex / 3))} is not finite.`,
      );
    }
    return;
  }
  let fixed = encoded[0] | (encoded[1] << 8) | (encoded[2] << 16);
  if ((fixed & 0x800000) !== 0) fixed -= 0x1000000;
  const value = fixed / (2 ** plan.fractionalBits);
  if (!Number.isFinite(value) || Math.abs(value) > HALF_FLOAT_MAX) {
    throw new Error(
      `SPZ position ${formatCount(Math.floor(scalarIndex / 3))} exceeds the viewer range.`,
    );
  }
}

function validateSpzRotation(encoded, version, splatIndex) {
  let squaredNorm = 0;
  if (version < 3) {
    for (let component = 0; component < 3; component++) {
      const value = encoded[component] / 127.5 - 1;
      squaredNorm += value * value;
    }
    const quantizationError = 1 / 255;
    const maximumSquaredNorm = 1 + 2 * Math.sqrt(3) * quantizationError +
      3 * quantizationError * quantizationError;
    if (squaredNorm > maximumSquaredNorm) {
      throw new Error(`SPZ rotation ${formatCount(splatIndex)} exceeds its quantization bound.`);
    }
    return;
  }

  let bits = (encoded[0] | (encoded[1] << 8) | (encoded[2] << 16) |
    (encoded[3] << 24)) >>> 0;
  const largest = bits >>> 30;
  const magnitudeMask = (1 << 9) - 1;
  for (let component = 3; component >= 0; component--) {
    if (component === largest) continue;
    const value = Math.SQRT1_2 * (bits & magnitudeMask) / magnitudeMask;
    squaredNorm += value * value;
    bits >>>= 10;
  }
  if (squaredNorm > 1) {
    throw new Error(`SPZ rotation ${formatCount(splatIndex)} has invalid smallest-three data.`);
  }
}

function validateSpzHeader(prefix) {
  const header = new DataView(prefix.buffer, prefix.byteOffset, prefix.byteLength);
  const version = header.getUint32(4, true);
  const count = header.getUint32(8, true);
  const shDegree = header.getUint8(12);
  const fractionalBits = header.getUint8(13);
  const flags = header.getUint8(14);
  const reserved = header.getUint8(15);
  if (header.getUint32(0, true) !== 0x5053474e || version < 1 || version > 3) {
    throw new Error("That SPZ file has an invalid header.");
  }
  if (count < 1) throw new Error("That SPZ file has no splats.");
  if (count > MAX_LOCAL_SPLATS) {
    throw new Error(`Local scenes are limited to ${formatCount(MAX_LOCAL_SPLATS)} splats.`);
  }
  if (shDegree > 3 || (version !== 1 && fractionalBits > 23) ||
      (flags & ~0x81) !== 0 || reserved !== 0) {
    throw new Error("That SPZ file uses unsupported header values.");
  }
  const shBytes = [0, 9, 24, 45][shDegree];
  const bytesPerSplat = (version === 1 ? 6 : 9) + 1 + 3 + 3 +
    (version >= 3 ? 4 : 3) + shBytes + ((flags & 0x80) !== 0 ? 6 : 0);
  const decodedBytes = 16 + count * bytesPerSplat;
  if (decodedBytes > MAX_SPZ_DECODED_BYTES) {
    throw new Error(`Decoded SPZ data is limited to ${formatBytes(MAX_SPZ_DECODED_BYTES)}.`);
  }
  const rotationBytes = version >= 3 ? 4 : 3;
  const positionBytesPerSplat = version === 1 ? 6 : 9;
  const rotationOffset = 16 + count * (positionBytesPerSplat + 1 + 3 + 3);
  const rotationEnd = rotationOffset + count * rotationBytes;
  const lodCountOffset = rotationEnd + count * shBytes;
  const hasLod = (flags & 0x80) !== 0;
  const lodCountEnd = lodCountOffset + (hasLod ? count * 2 : 0);
  const lodStartOffset = lodCountEnd;
  return {
    count,
    decodedBytes,
    fractionalBits,
    positionBytesPerSplat,
    positionEnd: 16 + count * positionBytesPerSplat,
    positionOffset: 16,
    positionScalarBytes: version === 1 ? 2 : 3,
    hasLod,
    lodCountEnd,
    lodCountOffset,
    lodStartEnd: lodStartOffset + (hasLod ? count * 4 : 0),
    lodStartOffset,
    rotationBytes,
    rotationEnd,
    rotationOffset,
    version,
  };
}

async function validateSpzContainer(file, shouldCancel) {
  await readSpzGzip(file, shouldCancel);
}

async function validateSplatContainer(file, shouldCancel) {
  if (file.size % 32 !== 0) throw new Error("That SPLAT file has an invalid size.");
  const count = file.size / 32;
  if (count < 1) throw new Error("That SPLAT file has no splats.");
  if (count > MAX_LOCAL_SPLATS) {
    throw new Error(`Local scenes are limited to ${formatCount(MAX_LOCAL_SPLATS)} splats.`);
  }
  const recordsPerBatch = (4 * 1024 * 1024) / 32;
  for (let first = 0; first < count; first += recordsPerBatch) {
    throwIfCanceled(shouldCancel);
    const batchCount = Math.min(recordsPerBatch, count - first);
    const start = first * 32;
    const bytes = new Uint8Array(
      await file.slice(start, start + batchCount * 32).arrayBuffer(),
    );
    const view = new DataView(bytes.buffer, bytes.byteOffset, bytes.byteLength);
    for (let record = 0; record < batchCount; record++) {
      const offset = record * 32;
      const position = [0, 4, 8].map((component) => view.getFloat32(offset + component, true));
      const scale = [12, 16, 20].map((component) => view.getFloat32(offset + component, true));
      const index = first + record;
      if (!position.every((value) => Number.isFinite(value) && Math.abs(value) <= HALF_FLOAT_MAX)) {
        throw new Error(`SPLAT record ${formatCount(index)} has an invalid position.`);
      }
      if (!scalesFitSpark(scale)) {
        throw new Error(`SPLAT record ${formatCount(index)} has an invalid scale.`);
      }
      let squaredNorm = 0;
      for (let component = 28; component < 32; component++) {
        const value = (bytes[offset + component] - 128) / 128;
        squaredNorm += value * value;
      }
      if (squaredNorm <= 1e-12) {
        throw new Error(`SPLAT record ${formatCount(index)} has an invalid rotation.`);
      }
    }
    await yieldForCancellation(shouldCancel);
  }
}

async function validateZipEntryData(file, entry, dataOffset, dataEnd, shouldCancel) {
  let stream = file.slice(dataOffset, dataEnd).stream();
  if (entry.method === 8) {
    let decompressor;
    try {
      decompressor = new DecompressionStream("deflate-raw");
    } catch {
      throw new Error("This browser cannot safely inspect deflate ZIP/SOG entries.");
    }
    stream = stream.pipeThrough(decompressor);
  }

  const reader = stream.getReader();
  const captureLimit = entry.name.split("/").pop() === "meta.json"
    ? MAX_SOG_METADATA_BYTES + 1
    : SOG_ENTRY_PREFIX_BYTES;
  const prefix = new Uint8Array(Math.min(entry.expandedBytes, captureLimit));
  let prefixOffset = 0;
  let crc = 0xffffffff;
  let expandedBytes = 0;
  try {
    while (true) {
      let chunk;
      try {
        chunk = await reader.read();
      } catch {
        throw new Error("That ZIP/SOG archive has invalid compressed data.");
      }
      const { done, value } = chunk;
      if (done) break;
      throwIfCanceled(shouldCancel);
      expandedBytes += value.byteLength;
      if (expandedBytes > entry.expandedBytes) {
        throw new Error("A ZIP/SOG entry expands beyond its directory size.");
      }
      if (prefixOffset < prefix.length) {
        const copied = Math.min(value.byteLength, prefix.length - prefixOffset);
        prefix.set(value.subarray(0, copied), prefixOffset);
        prefixOffset += copied;
      }
      for (const byte of value) {
        crc = CRC32_TABLE[(crc ^ byte) & 0xff] ^ (crc >>> 8);
      }
    }
  } finally {
    try { await reader.cancel(); } catch {}
  }
  if (expandedBytes !== entry.expandedBytes) {
    throw new Error("A ZIP/SOG entry has an incorrect expanded size.");
  }
  if ((crc ^ 0xffffffff) >>> 0 !== entry.crc32) {
    throw new Error("A ZIP/SOG entry has an incorrect checksum.");
  }
  return prefix;
}

async function validateKsplatContainer(file, shouldCancel) {
  const headerBytes = 4096;
  const sectionBytes = 1024;
  if (file.size < headerBytes) throw new Error("That KSPLAT file has a truncated header.");
  const headerBuffer = await file.slice(0, headerBytes).arrayBuffer();
  const header = new DataView(headerBuffer);
  const sectionCount = header.getUint32(4, true);
  const declaredCount = header.getUint32(16, true);
  const compression = header.getUint16(20, true);
  if (header.getUint8(0) !== 0 || header.getUint8(1) < 1 || compression > 2 ||
      sectionCount < 1 || sectionCount > 4096) {
    throw new Error("That KSPLAT file has unsupported header values.");
  }
  if (declaredCount < 1 || declaredCount > MAX_LOCAL_SPLATS) {
    throw new Error(`Local scenes are limited to ${formatCount(MAX_LOCAL_SPLATS)} splats.`);
  }
  if (sectionCount !== 1) {
    throw new Error("The viewer supports one KSPLAT section per file.");
  }
  const shMinimum = header.getFloat32(36, true) || -1.5;
  const shMaximum = header.getFloat32(40, true) || 1.5;
  if (compression === 2 &&
      (!Number.isFinite(shMinimum) || !Number.isFinite(shMaximum) ||
       shMinimum > shMaximum || !Number.isFinite(shMaximum - shMinimum) ||
       !valuesFitFloat32([shMinimum, shMaximum]))) {
    throw new Error("That KSPLAT file has an invalid SH range.");
  }
  const directoryEnd = headerBytes + sectionCount * sectionBytes;
  if (directoryEnd > file.size) throw new Error("That KSPLAT file has a truncated section directory.");
  const sectionsBuffer = await file.slice(headerBytes, directoryEnd).arrayBuffer();
  const sections = new DataView(sectionsBuffer);
  const shComponents = [0, 9, 24, 45];
  const fixedBytes = [44, 24, 24];
  const shComponentBytes = [4, 2, 1];
  let dataOffset = directoryEnd;
  let totalSplats = 0;
  for (let index = 0; index < sectionCount; index++) {
    if (index > 0 && index % 128 === 0) await yieldForCancellation(shouldCancel);
    const offset = index * sectionBytes;
    const count = sections.getUint32(offset, true);
    const capacity = sections.getUint32(offset + 4, true);
    const bucketSize = sections.getUint32(offset + 8, true);
    const bucketCount = sections.getUint32(offset + 12, true);
    const bucketBlockSize = sections.getFloat32(offset + 16, true);
    const bucketBytes = sections.getUint16(offset + 20, true);
    const encodedScaleRange = sections.getUint32(offset + 24, true);
    const scaleRange = encodedScaleRange || 32_767;
    const fullBuckets = sections.getUint32(offset + 32, true);
    const partialBuckets = sections.getUint32(offset + 36, true);
    const shDegree = sections.getUint16(offset + 40, true);
    const compressedBucketsInvalid = compression > 0 &&
      (bucketCount < 1 || bucketCount > MAX_LOCAL_SPLATS || bucketSize < 1 ||
        bucketBytes < 12 || bucketBytes % 4 !== 0 ||
        !Number.isFinite(bucketBlockSize) || bucketBlockSize <= 0 ||
        fullBuckets + partialBuckets !== bucketCount || fullBuckets * bucketSize > count);
    const rawBucketsInvalid = compression === 0 &&
      (bucketCount !== 0 || bucketBytes !== 0 || fullBuckets !== 0 || partialBuckets !== 0);
    if (count > capacity || capacity > MAX_LOCAL_SPLATS || shDegree > 3 ||
        compressedBucketsInvalid || rawBucketsInvalid) {
      throw new Error("That KSPLAT file has invalid section metadata.");
    }
    totalSplats += count;
    if (totalSplats > MAX_LOCAL_SPLATS) {
      throw new Error(`Local scenes are limited to ${formatCount(MAX_LOCAL_SPLATS)} splats.`);
    }
    const metadataBytes = partialBuckets * 4;
    const bucketStorageBytes = bucketBytes * bucketCount + metadataBytes;
    const bytesPerSplat = fixedBytes[compression] +
      shComponents[shDegree] * shComponentBytes[compression];
    const sectionStorageBytes = bucketStorageBytes + bytesPerSplat * capacity;
    if (!Number.isSafeInteger(sectionStorageBytes) ||
        dataOffset + sectionStorageBytes > file.size) {
      throw new Error("That KSPLAT file has a truncated section.");
    }
    let partialLengths = new Uint32Array();
    if (compression === 0) {
      // Raw sections do not use center buckets.
    } else if (partialBuckets > 0) {
      const lengthsBuffer = await file.slice(dataOffset, dataOffset + metadataBytes).arrayBuffer();
      partialLengths = new Uint32Array(lengthsBuffer);
      let represented = fullBuckets * bucketSize;
      for (const length of partialLengths) {
        if (length < 1 || length > bucketSize) {
          throw new Error("That KSPLAT file has an invalid partial bucket.");
        }
        represented += length;
      }
      if (represented !== count) {
        throw new Error("That KSPLAT file has inconsistent bucket counts.");
      }
    } else if (fullBuckets * bucketSize !== count) {
      throw new Error("That KSPLAT file has inconsistent bucket counts.");
    }

    const bucketCenters = new Float32Array(bucketCount * 3);
    if (compression > 0) {
      const bucketsOffset = dataOffset + metadataBytes;
      const bucketsPerBatch = Math.max(1, Math.floor((4 * 1024 * 1024) / bucketBytes));
      for (let first = 0; first < bucketCount; first += bucketsPerBatch) {
        throwIfCanceled(shouldCancel);
        const batchCount = Math.min(bucketsPerBatch, bucketCount - first);
        const start = bucketsOffset + first * bucketBytes;
        const bytes = await file.slice(start, start + batchCount * bucketBytes).arrayBuffer();
        const view = new DataView(bytes);
        for (let bucket = 0; bucket < batchCount; bucket++) {
          for (let axis = 0; axis < 3; axis++) {
            const value = view.getFloat32(bucket * bucketBytes + axis * 4, true);
            if (!Number.isFinite(value)) {
              throw new Error(`KSPLAT bucket ${formatCount(first + bucket)} has an invalid center.`);
            }
            bucketCenters[(first + bucket) * 3 + axis] = value;
          }
        }
        await yieldForCancellation(shouldCancel);
      }
    }

    const scaleOffset = compression === 0 ? 12 : 6;
    const rotationOffset = compression === 0 ? 24 : 12;
    const shOffset = compression === 0 ? 44 : 24;
    const dataBase = dataOffset + bucketStorageBytes;
    const recordsPerBatch = Math.max(1, Math.floor((4 * 1024 * 1024) / bytesPerSplat));
    let bucketIndex = 0;
    let bucketEnd = compression === 0 ? 0 :
      (fullBuckets > 0 ? bucketSize : partialLengths[0]);
    for (let first = 0; first < count; first += recordsPerBatch) {
      throwIfCanceled(shouldCancel);
      const batchCount = Math.min(recordsPerBatch, count - first);
      const start = dataBase + first * bytesPerSplat;
      const bytes = await file.slice(start, start + batchCount * bytesPerSplat).arrayBuffer();
      const view = new DataView(bytes);
      for (let record = 0; record < batchCount; record++) {
        const splatIndex = first + record;
        const base = record * bytesPerSplat;
        if (compression > 0) {
          while (splatIndex >= bucketEnd) {
            bucketIndex++;
            const nextLength = bucketIndex < fullBuckets
              ? bucketSize
              : partialLengths[bucketIndex - fullBuckets];
            bucketEnd += nextLength;
          }
        }

        for (let axis = 0; axis < 3; axis++) {
          const value = compression === 0
            ? view.getFloat32(base + axis * 4, true)
            : bucketCenters[bucketIndex * 3 + axis] +
              (view.getUint16(base + axis * 2, true) - scaleRange) *
                bucketBlockSize / 2 / scaleRange;
          if (!Number.isFinite(value) || Math.abs(value) > HALF_FLOAT_MAX) {
            throw new Error(`KSPLAT splat ${formatCount(splatIndex)} has an invalid position.`);
          }
        }

        const scales = [];
        for (let axis = 0; axis < 3; axis++) {
          scales.push(compression === 0
            ? view.getFloat32(base + scaleOffset + axis * 4, true)
            : halfToNumber(view.getUint16(base + scaleOffset + axis * 2, true)));
        }
        if (!scalesFitSpark(scales)) {
          throw new Error(`KSPLAT splat ${formatCount(splatIndex)} has an invalid scale.`);
        }

        let rotationNorm = 0;
        for (let component = 0; component < 4; component++) {
          const value = compression === 0
            ? view.getFloat32(base + rotationOffset + component * 4, true)
            : halfToNumber(view.getUint16(base + rotationOffset + component * 2, true));
          rotationNorm += value * value;
        }
        if (!Number.isFinite(rotationNorm) || rotationNorm <= 1e-12) {
          throw new Error(`KSPLAT splat ${formatCount(splatIndex)} has an invalid rotation.`);
        }

        if (compression < 2) {
          const componentBytes = shComponentBytes[compression];
          for (let component = 0; component < shComponents[shDegree]; component++) {
            const value = compression === 0
              ? view.getFloat32(base + shOffset + component * componentBytes, true)
              : halfToNumber(view.getUint16(
                base + shOffset + component * componentBytes,
                true,
              ));
            if (!Number.isFinite(value)) {
              throw new Error(`KSPLAT splat ${formatCount(splatIndex)} has invalid SH data.`);
            }
          }
        }
      }
      await yieldForCancellation(shouldCancel);
    }
    dataOffset += sectionStorageBytes;
  }
  if (totalSplats < 1) throw new Error("That KSPLAT file has no splats.");
  if (totalSplats !== declaredCount) {
    throw new Error("That KSPLAT file has inconsistent splat counts.");
  }
  if (dataOffset !== file.size) throw new Error("That KSPLAT file has trailing data.");
}

async function validateZipContainer(file, shouldCancel) {
  const minimumEocdBytes = 22;
  if (file.size < minimumEocdBytes) throw new Error("That ZIP/SOG archive is incomplete.");
  const tailBytes = Math.min(file.size, minimumEocdBytes + 65_535);
  const tailOffset = file.size - tailBytes;
  const tail = new Uint8Array(await file.slice(tailOffset).arrayBuffer());
  const tailView = new DataView(tail.buffer, tail.byteOffset, tail.byteLength);
  let eocd = -1;
  for (let offset = tail.length - minimumEocdBytes; offset >= 0; offset--) {
    if (tailView.getUint32(offset, true) !== 0x06054b50) continue;
    const commentBytes = tailView.getUint16(offset + 20, true);
    if (offset + minimumEocdBytes + commentBytes === tail.length) {
      eocd = offset;
      break;
    }
  }
  if (eocd < 0) throw new Error("That ZIP/SOG archive has no valid directory record.");

  const disk = tailView.getUint16(eocd + 4, true);
  const directoryDisk = tailView.getUint16(eocd + 6, true);
  const entriesOnDisk = tailView.getUint16(eocd + 8, true);
  const entryCount = tailView.getUint16(eocd + 10, true);
  const directoryBytes = tailView.getUint32(eocd + 12, true);
  const directoryOffset = tailView.getUint32(eocd + 16, true);
  if (disk !== 0 || directoryDisk !== 0 || entriesOnDisk !== entryCount) {
    throw new Error("Multi-disk ZIP/SOG archives are not supported.");
  }
  if (entryCount === 0 || entryCount === 0xffff || directoryBytes === 0xffffffff ||
      directoryOffset === 0xffffffff) {
    throw new Error("That ZIP/SOG archive has an unsupported ZIP64 or empty directory.");
  }
  if (entryCount > MAX_ZIP_ENTRIES) {
    throw new Error(`ZIP/SOG archives are limited to ${formatCount(MAX_ZIP_ENTRIES)} entries.`);
  }
  if (directoryBytes > MAX_ZIP_CENTRAL_BYTES) {
    throw new Error(`The ZIP/SOG directory exceeds ${formatBytes(MAX_ZIP_CENTRAL_BYTES)}.`);
  }
  const eocdOffset = tailOffset + eocd;
  if (directoryOffset + directoryBytes !== eocdOffset) {
    throw new Error("That ZIP/SOG archive has inconsistent directory offsets.");
  }

  const directory = new Uint8Array(
    await file.slice(directoryOffset, directoryOffset + directoryBytes).arrayBuffer(),
  );
  const view = new DataView(directory.buffer, directory.byteOffset, directory.byteLength);
  const utf8Decoder = new TextDecoder("utf-8", { fatal: true });
  const entries = [];
  const names = new Set();
  const portableNames = new Set();
  let offset = 0;
  let compressedTotal = 0;
  let expandedTotal = 0;
  for (let entry = 0; entry < entryCount; entry++) {
    if (entry > 0 && entry % 128 === 0) await yieldForCancellation(shouldCancel);
    if (offset + 46 > directory.length || view.getUint32(offset, true) !== 0x02014b50) {
      throw new Error("That ZIP/SOG archive has an invalid directory entry.");
    }
    const flags = view.getUint16(offset + 8, true);
    const method = view.getUint16(offset + 10, true);
    const crc32 = view.getUint32(offset + 16, true);
    const compressedBytes = view.getUint32(offset + 20, true);
    const expandedBytes = view.getUint32(offset + 24, true);
    const filenameBytes = view.getUint16(offset + 28, true);
    const extraBytes = view.getUint16(offset + 30, true);
    const commentBytes = view.getUint16(offset + 32, true);
    const startDisk = view.getUint16(offset + 34, true);
    const localOffset = view.getUint32(offset + 42, true);
    const entryBytes = 46 + filenameBytes + extraBytes + commentBytes;
    if (compressedBytes === 0xffffffff || expandedBytes === 0xffffffff ||
        localOffset === 0xffffffff) {
      throw new Error("ZIP64 entries are not supported.");
    }
    if (startDisk !== 0 || localOffset >= directoryOffset || offset + entryBytes > directory.length) {
      throw new Error("That ZIP/SOG archive has an invalid entry location.");
    }
    if ((flags & 1) !== 0) throw new Error("Encrypted ZIP/SOG archives are not supported.");
    if (method !== 0 && method !== 8) {
      throw new Error("ZIP/SOG entries must use stored or deflate compression.");
    }
    const allowedFlags = method === 8 ? 0x080e : 0x0808;
    if ((flags & (~allowedFlags & 0xffff)) !== 0) {
      throw new Error("That ZIP/SOG archive uses unsupported entry flags.");
    }
    const nameBytes = directory.subarray(offset + 46, offset + 46 + filenameBytes);
    let name;
    try {
      if ((flags & 0x0800) === 0 && nameBytes.some((byte) => byte > 0x7f)) {
        throw new Error("non-ASCII legacy name");
      }
      name = utf8Decoder.decode(nameBytes);
    } catch {
      throw new Error("ZIP/SOG entry names must use ASCII or valid UTF-8.");
    }
    if (!name || name.includes("\\") || name.startsWith("/") ||
        name.includes("//") || name.split("/").some((part) => part === "." || part === "..") ||
        name.includes("\0")) {
      throw new Error("That ZIP/SOG archive has an unsafe entry name.");
    }
    const portableName = name.toLowerCase();
    if (names.has(name) || portableNames.has(portableName)) {
      throw new Error("That ZIP/SOG archive has a duplicate entry name.");
    }
    names.add(name);
    portableNames.add(portableName);
    if (method === 0 && compressedBytes !== expandedBytes) {
      throw new Error("A stored ZIP/SOG entry has inconsistent sizes.");
    }
    compressedTotal += compressedBytes;
    expandedTotal += expandedBytes;
    if (expandedTotal > MAX_ZIP_EXPANDED_BYTES) {
      throw new Error(`ZIP/SOG content is limited to ${formatBytes(MAX_ZIP_EXPANDED_BYTES)} expanded.`);
    }
    entries.push({
      compressedBytes,
      crc32,
      expandedBytes,
      flags,
      localOffset,
      method,
      name,
      nameBytes: new Uint8Array(nameBytes),
    });
    offset += entryBytes;
  }
  if (offset !== directory.length) {
    throw new Error("That ZIP/SOG archive has trailing directory data.");
  }
  const expansionRatio = expandedTotal / Math.max(compressedTotal, 1);
  if (expansionRatio > MAX_ZIP_EXPANSION_RATIO) {
    throw new Error(`The ZIP/SOG expansion ratio exceeds ${MAX_ZIP_EXPANSION_RATIO}:1.`);
  }

  let previousRecordEnd = 0;
  const orderedEntries = entries.sort((left, right) => left.localOffset - right.localOffset);
  for (let index = 0; index < orderedEntries.length; index++) {
    if (index > 0 && index % 128 === 0) await yieldForCancellation(shouldCancel);
    const entry = orderedEntries[index];
    if (entry.localOffset !== previousRecordEnd) {
      const problem = entry.localOffset < previousRecordEnd ? "overlapping" : "unexplained";
      throw new Error(`That ZIP/SOG archive has ${problem} data before an entry.`);
    }
    const fixed = new Uint8Array(await file.slice(entry.localOffset, entry.localOffset + 30)
      .arrayBuffer());
    if (fixed.byteLength !== 30) {
      throw new Error("That ZIP/SOG archive has a truncated local header.");
    }
    const local = new DataView(fixed.buffer, fixed.byteOffset, fixed.byteLength);
    if (local.getUint32(0, true) !== 0x04034b50) {
      throw new Error("That ZIP/SOG archive has an invalid local header.");
    }
    const localFlags = local.getUint16(6, true);
    const localMethod = local.getUint16(8, true);
    const localCrc32 = local.getUint32(14, true);
    const localCompressedBytes = local.getUint32(18, true);
    const localExpandedBytes = local.getUint32(22, true);
    const localNameBytes = local.getUint16(26, true);
    const localExtraBytes = local.getUint16(28, true);
    const dataOffset = entry.localOffset + 30 + localNameBytes + localExtraBytes;
    const dataEnd = dataOffset + entry.compressedBytes;
    if (dataOffset > directoryOffset || dataEnd > directoryOffset) {
      throw new Error("That ZIP/SOG archive has an invalid local data range.");
    }
    if (localFlags !== entry.flags || localMethod !== entry.method) {
      throw new Error("That ZIP/SOG archive has inconsistent local entry metadata.");
    }
    const localName = new Uint8Array(
      await file.slice(entry.localOffset + 30, entry.localOffset + 30 + localNameBytes)
        .arrayBuffer(),
    );
    if (localName.byteLength !== entry.nameBytes.byteLength ||
        localName.some((byte, index) => byte !== entry.nameBytes[index])) {
      throw new Error("That ZIP/SOG archive has inconsistent local entry names.");
    }
    const usesDescriptor = (entry.flags & 0x0008) !== 0;
    const sizeMatches = (localValue, centralValue) =>
      localValue === centralValue || (usesDescriptor && localValue === 0);
    if (!sizeMatches(localCompressedBytes, entry.compressedBytes) ||
        !sizeMatches(localExpandedBytes, entry.expandedBytes) ||
        !sizeMatches(localCrc32, entry.crc32)) {
      throw new Error("That ZIP/SOG archive has inconsistent local entry sizes.");
    }
    const nextOffset = index + 1 < orderedEntries.length
      ? orderedEntries[index + 1].localOffset
      : directoryOffset;
    let recordEnd = dataEnd;
    if (usesDescriptor) {
      const descriptorBytes = nextOffset - dataEnd;
      if (descriptorBytes !== 12 && descriptorBytes !== 16) {
        throw new Error("That ZIP/SOG archive has an invalid data descriptor.");
      }
      const bytes = new Uint8Array(await file.slice(dataEnd, nextOffset).arrayBuffer());
      const descriptor = new DataView(bytes.buffer, bytes.byteOffset, bytes.byteLength);
      const valueOffset = descriptorBytes === 16 ? 4 : 0;
      if ((descriptorBytes === 16 && descriptor.getUint32(0, true) !== 0x08074b50) ||
          descriptor.getUint32(valueOffset, true) !== entry.crc32 ||
          descriptor.getUint32(valueOffset + 4, true) !== entry.compressedBytes ||
          descriptor.getUint32(valueOffset + 8, true) !== entry.expandedBytes) {
        throw new Error("That ZIP/SOG archive has an inconsistent data descriptor.");
      }
      recordEnd = nextOffset;
    }
    if (recordEnd !== nextOffset) {
      throw new Error("That ZIP/SOG archive has unexplained data between entries.");
    }
    entry.prefix = await validateZipEntryData(file, entry, dataOffset, dataEnd, shouldCancel);
    previousRecordEnd = recordEnd;
  }
  validateSogMetadata(entries);
}

export async function validateLocalFileInProcess(
  file,
  localFileTypes,
  shouldCancel = () => false,
) {
  throwIfCanceled(shouldCancel);
  if (!(file instanceof File)) throw new Error("Choose one local splat file.");
  if (file.size <= 0) throw new Error("That file is empty. Choose a non-empty splat file.");
  if (file.size > MAX_LOCAL_FILE_BYTES) {
    throw new Error(`That file is ${formatBytes(file.size)}. Local files are limited to ${formatBytes(MAX_LOCAL_FILE_BYTES)}.`);
  }
  const descriptor = localFileTypes.get(localExtension(file.name));
  if (!descriptor) {
    throw new Error("Unsupported file type. Choose PLY, SPZ, SPLAT, KSPLAT, SOG, or ZIP.");
  }
  const extension = localExtension(file.name);
  if (extension === "ply") await validatePlyContainer(file, shouldCancel);
  if (extension === "spz") await validateSpzContainer(file, shouldCancel);
  if (extension === "splat") await validateSplatContainer(file, shouldCancel);
  if (extension === "ksplat") await validateKsplatContainer(file, shouldCancel);
  if (descriptor.fmt === "SOG") await validateZipContainer(file, shouldCancel);
  throwIfCanceled(shouldCancel);
  return descriptor;
}

let activeValidation = null;
let validationGeneration = 0;
let workerState = null;

function abortError() {
  const error = new Error("The local file check was canceled.");
  error.name = "AbortError";
  return error;
}

export function cancelLocalFileValidation() {
  validationGeneration++;
  if (!activeValidation) return;
  const validation = activeValidation;
  activeValidation = null;
  workerState?.worker.postMessage({ id: validation.id, kind: "cancel" });
  validation.reject(abortError());
}

function createWorkerState() {
  if (typeof Worker !== "function") {
    throw new Error("This browser cannot safely inspect local files.");
  }
  const worker = new Worker(new URL("./local-file-validator-worker.js", import.meta.url), {
    name: "melkor-local-file-validator",
    type: "module",
  });
  let resolveReady;
  let rejectReady;
  const ready = new Promise((resolve, reject) => {
    resolveReady = resolve;
    rejectReady = reject;
  });
  const state = { ready, rejectReady, resolveReady, worker };
  worker.addEventListener("message", (event) => {
    const result = event.data;
    if (result?.kind === "ready") {
      state.resolveReady();
      return;
    }
    if (result?.kind !== "result" || activeValidation?.id !== result.id) return;
    const validation = activeValidation;
    activeValidation = null;
    if (result.ok === true) {
      validation.resolve(result.descriptor);
      return;
    }
    const message = typeof result.message === "string" && result.message.length > 0
      ? result.message
      : "The local file check failed safely.";
    validation.reject(new Error(message));
  });
  const fail = (message) => {
    if (workerState !== state) return;
    workerState = null;
    state.rejectReady(new Error(message));
    if (activeValidation) {
      const validation = activeValidation;
      activeValidation = null;
      validation.reject(new Error(message));
    }
    worker.terminate();
  };
  worker.addEventListener("error", () => fail("The local file check failed safely."));
  worker.addEventListener("messageerror", () => fail("The local file check returned invalid data."));
  return state;
}

function ensureWorkerState() {
  if (!workerState) workerState = createWorkerState();
  return workerState;
}

export async function prepareLocalFileValidation() {
  await ensureWorkerState().ready;
}

export async function validateLocalFile(file, localFileTypes) {
  cancelLocalFileValidation();
  const generation = validationGeneration;
  const state = ensureWorkerState();
  await state.ready;
  if (generation !== validationGeneration) throw abortError();

  return await new Promise((resolve, reject) => {
    const validation = { id: generation, reject, resolve };
    activeValidation = validation;
    try {
      state.worker.postMessage({
        file,
        id: validation.id,
        kind: "validate",
        localFileTypes: Array.from(localFileTypes.entries()),
      });
    } catch {
      if (activeValidation === validation) activeValidation = null;
      reject(new Error("The browser could not start the local file check."));
    }
  });
}

const severityRank = new Map([
  ["info", 0],
  ["warning", 1],
  ["severe", 2],
  ["fatal", 3],
]);

const minimumSeverity = new Map([
  ["LOSS_SH_DEGREE_TRUNCATED", "severe"],
  ["LOSS_SH_COEFFICIENTS_DROPPED", "severe"],
  ["LOSS_SH_COEFFICIENTS_CLAMPED", "severe"],
  ["LOSS_SH_ROTATION_NOT_APPLIED", "severe"],
  ["LOSS_SCENE_GRAPH_FLATTENED", "info"],
  ["LOSS_NODE_NAME_DROPPED", "info"],
  ["LOSS_INSTANCE_EXPANDED", "info"],
  ["LOSS_MATERIAL_APPROXIMATED", "severe"],
  ["LOSS_TEXTURE_BAKED", "severe"],
  ["LOSS_ANTIALIASING_METADATA_DROPPED", "severe"],
  ["LOSS_COLOR_SPACE_ASSUMED", "severe"],
  ["LOSS_COLOR_SPACES_CONFLICT", "severe"],
  ["LOSS_COLOR_CLAMPED", "warning"],
  ["LOSS_COLOR_SPACE_METADATA_DROPPED", "severe"],
  ["LOSS_COORDINATE_METADATA_DROPPED", "severe"],
  ["LOSS_PROVENANCE_DROPPED", "info"],
  ["LOSS_ATTRIBUTION_DROPPED", "severe"],
  ["LOSS_QUANTIZATION_APPLIED", "warning"],
  ["LOSS_OPACITY_CLAMPED", "warning"],
  ["LOSS_SCALE_CLAMPED", "severe"],
  ["LOSS_NONFINITE_REPAIRED", "severe"],
  ["LOSS_INVALID_SPLAT_DROPPED", "severe"],
  ["LOSS_UNKNOWN_PROPERTY_DROPPED", "severe"],
  ["LOSS_METADATA_DROPPED", "info"],
  ["LOSS_VERTEX_NORMALS_DROPPED", "severe"],
  ["LOSS_QUATERNION_NORMALIZED", "info"],
  ["LOSS_EXTENSION_DROPPED", "severe"],
  ["LOSS_EXTENSION_DECLARATION_DROPPED", "info"],
  ["LOSS_GLTF_CONTENT_DROPPED", "severe"],
  ["LOSS_PRECISION_REDUCED", "warning"],
  ["LOSS_GAUSSIAN_ATTRIBUTES_GENERATED", "severe"],
]);

function fail(message) {
  throw new Error(`invalid Melkor loss report: ${message}`);
}

function isRecord(value) {
  return value !== null && typeof value === "object" && !Array.isArray(value);
}

function hasExactKeys(value, expected) {
  const actual = Object.keys(value).sort();
  const wanted = [...expected].sort();
  return actual.length === wanted.length &&
    actual.every((key, index) => key === wanted[index]);
}

function nonemptyString(value) {
  return typeof value === "string" && value.length > 0;
}

function validateEndpoint(endpoint, name, expectedFormat) {
  if (!isRecord(endpoint) || !hasExactKeys(endpoint, ["format", "profile"])) {
    fail(`${name} must contain only format and profile`);
  }
  if (!nonemptyString(endpoint.format) || !nonemptyString(endpoint.profile)) {
    fail(`${name} format and profile must be nonempty strings`);
  }
  if (expectedFormat && endpoint.format !== expectedFormat) {
    fail(`${name} format must be ${expectedFormat}`);
  }
}

export function validateLossReport(
  report,
  { inputFormat = null, outputFormat = null } = {},
) {
  if (!isRecord(report) || !hasExactKeys(
    report,
    ["schema_version", "input", "output", "items", "approved_codes"],
  )) {
    fail("the v1 document has unknown or missing fields");
  }
  if (report.schema_version !== 1) fail("schema_version must equal 1");
  validateEndpoint(report.input, "input", inputFormat);
  validateEndpoint(report.output, "output", outputFormat);

  if (!Array.isArray(report.approved_codes)) {
    fail("approved_codes must be an array");
  }
  const approvals = new Set();
  for (const code of report.approved_codes) {
    if (!minimumSeverity.has(code)) fail("approved_codes contains an unknown code");
    if (approvals.has(code)) fail("approved_codes contains a duplicate code");
    approvals.add(code);
  }

  if (!Array.isArray(report.items)) fail("items must be an array");
  const itemKeys = [
    "code",
    "severity",
    "source_feature",
    "target_constraint",
    "affected_splats",
    "remediation",
  ];
  for (const [index, item] of report.items.entries()) {
    if (!isRecord(item) || !hasExactKeys(item, itemKeys)) {
      fail(`items[${index}] has unknown or missing fields`);
    }
    const minimum = minimumSeverity.get(item.code);
    if (!minimum) fail(`items[${index}].code is unknown`);
    const rank = severityRank.get(item.severity);
    if (rank === undefined || rank < severityRank.get(minimum)) {
      fail(`items[${index}].severity is invalid for ${item.code}`);
    }
    for (const key of ["source_feature", "target_constraint", "remediation"]) {
      if (!nonemptyString(item[key])) fail(`items[${index}].${key} must be nonempty`);
    }
    if (!Number.isSafeInteger(item.affected_splats) || item.affected_splats < 0) {
      fail(`items[${index}].affected_splats must be a safe nonnegative integer`);
    }
    if (item.severity === "fatal") {
      fail(`items[${index}] contains a fatal loss after a successful conversion`);
    }
    if (item.severity === "severe" && !approvals.has(item.code)) {
      fail(`items[${index}] contains an unapproved severe loss`);
    }
  }

  return report;
}

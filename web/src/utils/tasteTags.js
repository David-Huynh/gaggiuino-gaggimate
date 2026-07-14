export const PROFILE_TASTE_TAGS = [
  { value: 'fruity', label: 'Fruity' },
  { value: 'citrus', label: 'Citrus' },
  { value: 'floral', label: 'Floral' },
  { value: 'sweet', label: 'Sweet' },
  { value: 'nutty_cocoa', label: 'Nutty / Cocoa' },
  { value: 'roasted', label: 'Roasted' },
  { value: 'spice', label: 'Spice' },
  { value: 'fermented', label: 'Fermented' },
];

export const FAULT_TASTE_TAGS = [
  { value: 'sour', label: 'Sour' },
  { value: 'green_vegetative', label: 'Green / Vegetative' },
  { value: 'bitter', label: 'Bitter' },
  { value: 'astringent_harsh', label: 'Astringent / Harsh' },
  { value: 'papery_stale', label: 'Papery / Stale' },
  { value: 'salty', label: 'Salty' },
];

export const TASTE_TAG_GROUPS = [
  { key: 'profile', label: 'Profile notes', tags: PROFILE_TASTE_TAGS },
  { key: 'imbalance', label: 'Imbalance', tags: FAULT_TASTE_TAGS },
];

export const TASTE_TAG_OPTIONS = [...PROFILE_TASTE_TAGS, ...FAULT_TASTE_TAGS];
export const TASTE_LEVELS = ['unspecified', 'low', 'medium', 'high'];
export const TASTE_LEVEL_LABELS = ['Any', 'Low', 'Medium', 'High'];

export const TASTE_TAG_LABELS = Object.fromEntries(
  TASTE_TAG_OPTIONS.map(tag => [tag.value, tag.label]),
);

const TASTE_TAG_VALUES = new Set(TASTE_TAG_OPTIONS.map(tag => tag.value));

export function normalizeTasteTag(value) {
  if (typeof value !== 'string') {
    return null;
  }
  const parsed = value
    .trim()
    .toLowerCase()
    .replace(/[^a-z0-9_ /-]+/g, '')
    .replace(/[\s/-]+/g, '_')
    .replace(/^_+|_+$/g, '');
  return TASTE_TAG_VALUES.has(parsed) ? parsed : null;
}

export function normalizeTasteTags(values) {
  const normalized = [];
  const seen = new Set();
  for (const value of Array.isArray(values) ? values : []) {
    const tag = normalizeTasteTag(value);
    if (!tag || seen.has(tag)) {
      continue;
    }
    normalized.push(tag);
    seen.add(tag);
  }
  return normalized;
}

export function normalizeNotesTasteFields(notes = {}) {
  const tasteTags = normalizeTasteTags(notes.tasteTags?.length ? notes.tasteTags : [notes.balanceTaste]);
  return {
    ...notes,
    tasteTags,
    balanceTaste: deriveLegacyBalanceTaste(tasteTags),
  };
}

export function deriveLegacyBalanceTaste(tags) {
  const normalized = normalizeTasteTags(tags);
  if (normalized.includes('sour')) {
    return 'sour';
  }
  if (normalized.includes('bitter') || normalized.includes('astringent_harsh')) {
    return 'bitter';
  }
  if (normalized.length) {
    return normalized[0];
  }
  return '';
}

export function formatTasteTags(tags, fallback = '-') {
  const normalized = normalizeTasteTags(tags);
  return normalized.length ? normalized.map(tag => TASTE_TAG_LABELS[tag] || tag).join(', ') : fallback;
}

# MoonRay LPE/AOV Code Report (Cycles Reference)

Date: 2026-02-24
Source tree: E:\blender_modify\openmoonray

## Scope
- Analyze how MoonRay implements LPE parsing, state machines, labels, and AOV accumulation.
- Provide reference points for designing Cycles LPE + AOV features.

## Architecture Overview
- LPE strings are parsed into an AST, compiled to NDF → DF automata, then optimized into a compact
  transition table used at render time.
- A single `lpe::StateMachine` is used for both Light AOVs and Visibility AOVs. They use separate
  schema ID ranges to avoid collisions.
- Material AOVs may reference LPEs by creating a LightAovs entry and checking `isValid()` before
  computing the material AOV.

Key files:
- `moonray\moonray\lib\rendering\lpe\StateMachine.h/.cc`
- `moonray\moonray\lib\rendering\lpe\osl\lpeparse.h/.cc`
- `moonray\moonray\lib\rendering\lpe\osl\lpexp.h/.cc`
- `moonray\moonray\lib\rendering\lpe\osl\automata.h`
- `moonray\moonray\lib\rendering\lpe\osl\optautomata.h`
- `moonray\moonray\lib\rendering\pbr\core\Aov.h/.cc`
- `moonray\moonray\lib\rendering\shading\AovLabels.hh`

## LPE Grammar and Tokens
Grammar summary (regex style):
- Concatenation, alternation `|`, grouping `(...)`, groups `<...>`, sets `[ ... ]`, negated sets `[^ ... ]`,
  wildcard `.`, repetition `*`, `+`, `{m,n}`.
- Custom labels use `'label'` syntax.

Token mapping (OSL labels):
- Event types: `C` camera, `L` light, `B` background, `V` volume, `O` emission object,
  `T` transmit, `R` reflect.
- Scattering: `D` diffuse, `G` glossy, `S` singular, `s` straight.
- `STOP` sentinel: `__stop__`.

Key files:
- `moonray\moonray\lib\rendering\lpe\osl\lpeparse.h`
- `moonray\moonray\lib\rendering\lpe\osl\closure.cc`

## State Machine Build and Transition
- `StateMachine::addExpression()` parses the LPE, stores `LPexp`, and records any `'label'` literals.
- `StateMachine::build()` compiles all expressions into a single optimized automaton.
- A special internal label `"placeholder"` is inserted to make `[^ ... ]` exclusions work even if no
  labels exist (required by OSL automata semantics).
- `transition()` applies event → scattering → label transitions, then always applies STOP.
  If no label is provided, it still transitions using the placeholder label.

Key files:
- `moonray\moonray\lib\rendering\lpe\StateMachine.cc`

## Labels and Encoding
Problem:
- Lobe labels are local to a material. LPEs need global label IDs for matching.

Solution:
- Encode material and LPE labels into a single integer:
  - Bit 31: transformed flag.
  - Bits [0..14]: material AOV label ID.
  - Bits [16..30]: LPE label ID.
- `aovEncodeLabels()` is called in the path integrator to encode labels for every BsdfLobe/Bssrdf/VolumeSubsurface.
- LPE matching uses the encoded LPE label bits (`aovDecodeLpeLabel()`).

Key files:
- `moonray\moonray\lib\rendering\shading\AovLabels.hh`
- `moonray\moonray\lib\rendering\pbr\integrator\PathIntegrator.cc`
- `moonray\moonray\lib\rendering\pbr\core\Aov.cc` (computeScatterEventLabelId)

## LPE Entry Creation and Aliases
`LightAovs::createEntry()`:
- Parses a prefix (currently `unoccluded;`).
- Replaces alias names with LPE strings.
- Expands labels based on material/lobe definitions in the render layer.
- Adds the expression to the shared state machine.

Alias mapping (examples):
- `diffuse` -> `CD[<L.>O]`
- `glossy` -> `CG[<L.>O]`
- `mirror` -> `CS[<L.>O]`
- `reflection` -> `C<RS>[DSG]+[<L.>O]`
- `translucent` -> `C<TD>[DSG]+[<L.>O]`
- `transmission` -> `C<TS>[DSG]+[<L.>O]`
- `emission` -> `CO`
- `caustic` -> `CD[S]+[<L.>O]`

Key files:
- `moonray\moonray\lib\rendering\pbr\core\Aov.cc` (replaceLpeAliases, parseFlagsFromLpePrefix)

## Label Expansion (Material and Lobe Labels)
Problem:
- When both material labels and lobe labels exist, a single `'material'` label does not match
  `material.lobe` combined labels.

Solution:
- `expandLpeLabels()` scans for `'label'` in the LPE and replaces it with a label orlist:
  `'mat'` -> `['mat''mat.lobeA''mat.lobeB'...]`
- The expansion map is built by scanning all materials in the render layer, collecting material labels
  and their lobe labels.

Key files:
- `moonray\moonray\lib\rendering\pbr\core\Aov.cc` (expandLpeLabels, buildLabelSubstitutions)

## Event Transitions
Event transitions are called by integrators:
- Camera: `cameraEventTransition()`.
- Surface scatter: `scatterEventTransition()` maps BsdfLobe type to EventType (R/T) and ScatteringType (D/G/S).
- Light hit: `lightEventTransition()` uses `Light::getLabelId()`.
- Emission:
  - Surface emission: `EVENT_TYPE_EMISSION` with material label.
  - Volume emission: `EVENT_TYPE_EMISSION` with volume label.
- Volume scatter: `EVENT_TYPE_VOLUME` with scattering type none.
- Extra AOV and Material AOV events are supported via `EVENT_TYPE_EXTRA` and `EVENT_TYPE_MATERIAL`.

Key files:
- `moonray\moonray\lib\rendering\pbr\core\Aov.cc`
- `moonray\moonray\lib\rendering\pbr\integrator\PathIntegratorVolume.cc`
- `moonray\moonray\lib\rendering\pbr\core\Scene.cc` (light/volume label assignment)

## AOV Accumulation
- `aovAccumLpeAovs()` is the core helper used by Light and Visibility AOV accumulation.
- Filters:
  - AVG uses “value”.
  - SUM/MIN/MAX use “sampleValue”.
- Prefix flags:
  - LPEs with `unoccluded;` use the prefix flag to select between occluded vs unoccluded values.
  - The integrators pass both “match” and “nonMatch” values when prefix flags are present.

Key files:
- `moonray\moonray\lib\rendering\pbr\core\Aov.cc`
- `moonray\moonray\lib\rendering\pbr\integrator\PathIntegratorMultiSampler.cc`

## Visibility AOVs
- Visibility AOVs are stored as `Vec2f` and match against the LPE state machine.
- Accumulation typically uses `(hitValue, 1.0)` (hit/miss plus attempt count).
- Visibility AOVs have their own schema ID range but share the same state machine.

Key files:
- `moonray\moonray\lib\rendering\pbr\core\Aov.h/.cc`
- `moonray\moonray\lib\rendering\pbr\handlers\RayHandlerUtils.cc`

## Cycles-Oriented Notes
- Consider a compact label encoding (material + lobe) to keep per-hit LPE evaluation cheap.
- Add a label-expansion pass so “material only” or “lobe only” LPEs still match combined labels.
- Prefix flags (like `unoccluded;`) are a useful way to express special-case LPE variants without new tokens.
- Material AOVs can reuse the LPE state machine as a filter gate without writing extra channels.


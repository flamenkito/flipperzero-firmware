#include "flooper_pattern.h"
#include "flooper_pattern_legacy.h"
#include <string.h>

static void render(FlooperReader* r) {
    uint16_t item = fp_get(r, 1, "render");
    r->out->master_volume = fp_real(r, fp_get(r, item, "master_volume"), 0, 1);
    if(!fp_bool(r, fp_get(r, item, "monophonic"))) fp_fail(r, FlooperPatternRenderPolicy);
    uint16_t policy = fp_get(r, item, "overlap_policy");
    if(fp_type(r, policy, JsonString) &&
       !json_min_equal(r->json, policy, "higher_priority_then_louder"))
        fp_fail(r, FlooperPatternRenderPolicy);
}

static void accents(FlooperReader* r) {
    uint16_t ui = fp_get(r, 1, "ui");
    if(!ui) return;
    uint16_t labels = fp_get(r, ui, "accent_counts");
    if(!labels) return;
    unsigned count = fp_array(r, labels, FLOOPER_COUNTS_MAX);
    for(unsigned i = 0; i < count; ++i) {
        uint16_t mask = 1u << fp_label(r, fp_at(r, labels, i));
        if(r->out->accent_mask & mask) fp_fail(r, FlooperPatternDuplicate);
        r->out->accent_mask |= mask;
    }
}

FlooperPatternError flooper_pattern_parse(
    const char* input,
    size_t length,
    FlooperPatternWorkspace* workspace,
    FlooperPattern* output) {
    if(!output) return FlooperPatternType;
    memset(output, 0, sizeof(*output));
    if(!workspace) return FlooperPatternType;
    JsonMinError json_error = json_min_parse(workspace, input, length);
    static const FlooperPatternError errors[] = {
        FlooperPatternOk,
        FlooperPatternJsonSyntax,
        FlooperPatternJsonSize,
        FlooperPatternJsonTokens,
        FlooperPatternJsonDepth,
        FlooperPatternJsonDuplicate,
        FlooperPatternNumber};
    if(json_error != JsonMinOk) return errors[json_error];
    FlooperReader r = {.json = workspace, .out = output, .error = FlooperPatternOk};
    uint16_t version_token = fp_get(&r, 1, "schema_version");
    uint16_t revision_token = fp_get(&r, 1, "schema_revision");
    uint32_t version = version_token ? fp_uint(&r, version_token, 0, UINT32_MAX) : 1;
    uint32_t revision = revision_token ? fp_uint(&r, revision_token, 0, UINT32_MAX) : 0;
    if(r.error == FlooperPatternOk) {
        if(version == 1 && !revision_token)
            flooper_pattern_legacy_v1(&r);
        else if(version == 2 && !revision_token)
            flooper_pattern_legacy_draft(&r);
        else if(version == 2 && revision == 1)
            fp_canonical(&r);
        else
            fp_fail(&r, FlooperPatternUnsupported);
        if(version == 2) render(&r);
        accents(&r);
        fp_finish(&r);
    }
    if(r.error != FlooperPatternOk) memset(output, 0, sizeof(*output));
    return r.error;
}

const char* flooper_pattern_error_name(FlooperPatternError error) {
    static const char* const names[] = {
        "Ok",
        "JsonSyntax",
        "JsonSize",
        "JsonTokens",
        "JsonDepth",
        "JsonDuplicate",
        "Number",
        "UnsupportedPattern",
        "Missing",
        "Type",
        "Integer",
        "Range",
        "String",
        "Limit",
        "Duplicate",
        "Reference",
        "DerivedTime",
        "Containment",
        "RenderPolicy"};
    return (unsigned)error < sizeof(names) / sizeof(names[0]) ? names[error] : "UnknownError";
}

#include "helper.hpp"
#define BRACES { }
const auto *text = u8R"tag(// not a comment
/* nor this */ { }
)tag";
const char *quoted = "\\\"//";
const char quote = '\'';
/* a real comment */

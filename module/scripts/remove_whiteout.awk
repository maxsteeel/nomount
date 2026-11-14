# remove_whiteout.awk - compute whiteout vpath for a .remove marker
# Input:  one .remove file path per line
# Output: one vpath per input (suffix stripped, partition mapped)

{
  f = $0
  prefix = mod "/"
  if (index(f, prefix) == 1) v = substr(f, length(prefix) + 1)
  else { v = f; f = prefix f }

  sub(/\/\.remove$/, "", v)

  if (substr(v, 1, 7) == "system/") {
    rest = substr(v, 8)
    idx = index(rest, "/")
    if (idx > 0) p = substr(rest, 1, idx - 1)
    else p = rest
    if (p == "vendor" || p == "system_ext" || p == "product" || p == "odm" ||
        p == "apex"   || p == "oem"         || p == "optics"  || p == "prism" ||
        p == "mi_ext" || p ~ /^my_/) {
      v = p substr(v, 8 + length(p))
    }
  }

  printf "/%s\n", v
}

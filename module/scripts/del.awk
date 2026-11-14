# del.awk - compute vpath for a path whose rule must be deleted
# Input:  one raw absolute path per line (files, dirs, .replace, .remove)
# Output: one vpath per input (marker suffix stripped, partition mapped)

{
  f = $0
  prefix = mod "/"
  if (index(f, prefix) == 1) v = substr(f, length(prefix) + 1)
  else { v = f; f = prefix f }

  base = v
  sub(/.*\//, "", base)
  if (base == ".replace" || base == ".remove")
    sub(/\/(\.replace|\.remove)$/, "", v)

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

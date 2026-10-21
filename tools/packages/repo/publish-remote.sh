#!/bin/sh
# Called over SSH by publish.sh. Only the dedicated repository is mutated.
set -eu
umask 022
base=/Volumes/Ahineya/RiplixRepo
expected=E3129F95-3ACD-4BD1-B261-35D449BB4A1D
actual=$(/usr/sbin/diskutil info -plist /Volumes/Ahineya | /usr/bin/plutil -extract VolumeUUID raw -)
[ "$actual" = "$expected" ] || { echo 'Repository SSD is absent or changed' >&2; exit 1; }
incoming=${1:?missing incoming directory}
case "$incoming" in "$base"/private/incoming.*) ;; *) echo 'Invalid incoming directory' >&2; exit 2;; esac
[ -d "$incoming" ] && [ ! -L "$incoming" ]
lock=$base/private/publish.lock
mkdir "$lock" || { echo 'Publication locked; inspect private/publish.lock before retrying' >&2; exit 1; }
work=$(mktemp -d "$base/private/publish.XXXXXX")
trap 'rm -rf "$work" "$incoming"; rmdir "$lock"' EXIT HUP INT TERM
engine=$HOME/.local/share/riplix-repo/pkg-engine
mkdir -p "$base/private/catalog" "$base/public/objects"
if [ ! -f "$base/private/signing.pem" ]; then
    (umask 077; openssl genpkey -algorithm RSA -pkeyopt rsa_keygen_bits:2048 -out "$base/private/signing.pem" 2>"$work/keygen.log")
fi
openssl rsa -in "$base/private/signing.pem" -noout -modulus | cut -d= -f2 | tr 'A-F' 'a-f' > "$base/public/repo.pub"
for archive in "$incoming"/*.rpk; do
    [ -f "$archive" ] || continue
    "$engine" inspect "$archive" > "$work/inspection"
    identity=$(head -1 "$work/inspection" | awk '{print $1 " " $3}')
    id=$(printf '%s\n' "$identity" | shasum -a 256 | cut -c1-24)
    cp "$archive" "$base/private/catalog/$id.rpk"
done
for archive in "$base/private/catalog"/*.rpk; do
    [ -f "$archive" ] || continue
    cp "$archive" "$work/$(basename "$archive")"
done
"$engine" repo "$work"
# Content-addressed libraries/grouping records are generated support packages.
# Retain them only while a current application/dependency still needs them.
# Iterate because dropping an old grouping record may orphan its children.
while :; do
    awk -F '\t' '
      NR>1 {
        names[NR]=$1; targets[NR]=$3
        n=split($7,deps,",")
        for(i=1;i<=n;i++) {
          split(deps[i],pair,"@")
          refs[pair[1] SUBSEP $3]=1; refs[pair[1] SUBSEP "any"]=1
        }
      }
      END {for(i in names) {
        name=names[i]; hash=name
        if(index(name,"fxlib-")==1) sub(/^fxlib-/,"",hash)
        else if(index(name,"bundle-")==1) sub(/^bundle-/,"",hash)
        else continue
        if(length(hash)==16 && hash~/^[0-9a-f]+$/ && !refs[name SUBSEP targets[i]])
          print name " " targets[i]
      }}' "$work/index.v1" > "$work/orphans"
    [ -s "$work/orphans" ] || break
    while read -r name target; do
        id=$(printf '%s [%s]\n' "$name" "$target" | shasum -a 256 | cut -c1-24)
        rm "$work/$id.rpk" "$base/private/catalog/$id.rpk"
    done < "$work/orphans"
    "$engine" repo "$work"
done
openssl dgst -sha256 -sign "$base/private/signing.pem" -out "$work/index.sig" "$work/index.v1"
"$engine" verify "$work/index.v1" "$work/index.sig" "$base/public/repo.pub"
for object in "$work/objects"/*.rpk; do
    [ -f "$object" ] || continue
    name=$(basename "$object")
    if [ -f "$base/public/objects/$name" ]; then
        cmp "$object" "$base/public/objects/$name"
    else
        cp "$object" "$base/public/objects/.$name.new"
        mv "$base/public/objects/.$name.new" "$base/public/objects/$name"
    fi
done
if [ -f "$base/public/index.v1" ]; then cp "$base/public/index.v1" "$base/private/previous-index.v1"; fi
cp "$work/index.v1" "$base/public/.index.v1.new"
cp "$work/index.sig" "$base/public/.index.sig.new"
mv "$base/public/.index.sig.new" "$base/public/index.sig"
mv "$base/public/.index.v1.new" "$base/public/index.v1"
# Bound public object retention to the current and one previous index.
awk -F '\t' 'NR>1 {print $6}' "$work/index.v1" > "$work/keep"
if [ -f "$base/private/previous-index.v1" ]; then awk -F '\t' 'NR>1 {print $6}' "$base/private/previous-index.v1" >> "$work/keep"; fi
for object in "$base/public/objects"/*.rpk; do
    [ -f "$object" ] || continue
    name=$(basename "$object")
    case "$name" in ????????????????????????.rpk)
        if ! grep -Fqx "objects/$name" "$work/keep"; then rm "$object"; fi;;
    esac
done
printf 'Published %s packages\n' "$(awk 'END {print NR-1}' "$work/index.v1")"

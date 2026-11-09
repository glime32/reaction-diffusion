#!/usr/bin/env bash
# Simule sans terminal (--export) et vérifie les images obtenues. Moins d'une seconde.
cd "$(dirname "$0")"
make -s rd || exit 1
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT
ok=0 total=0
resultat() { # description, commande...
  local desc="$1"; shift
  total=$((total + 1))
  if "$@" > /dev/null 2>&1; then ok=$((ok + 1)); echo "  ok    $desc"; else echo "  ÉCHEC $desc"; fi
}

simule() { # sortie.ppm|png, arguments de rd...
  local f="$1"; shift
  ./rd "$@" --export "$f" --taille 96x96 --echelle 1 --graine 7 2> /dev/null
}

couverte() { # image.ppm -> part des pixels où il y a du V (pas noirs), en millièmes
  tail -c +$(( $(head -n 3 "$1" | wc -c) + 1 )) "$1" | od -An -v -tu1 | awk '
    { for (i = 1; i <= NF; i++) { s += $i; if (++c == 3) { n++; if (s > 60) v++; s = c = 0 } } }
    END { printf "%d\n", 1000 * v / n }'
}
entre() { local p; p=$(couverte "$1"); ((p >= $2 && p <= $3)); }

./rd mitose --export "$tmp/taille.ppm" --taille 64x48 --echelle 3 --etapes 10 2> /dev/null
resultat "--taille 64x48 --echelle 3 : image 192×144"   test "$(sed -n 2p "$tmp/taille.ppm")" = "192 144"

simule "$tmp/a.ppm" corail --etapes 500
simule "$tmp/b.ppm" corail --etapes 500
./rd corail --export "$tmp/c.ppm" --taille 96x96 --echelle 1 --graine 8 --etapes 500 2> /dev/null
resultat "même graine : même image"                      cmp "$tmp/a.ppm" "$tmp/b.ppm"
resultat "autre graine : autre image"                    sh -c "! cmp -s '$tmp/a.ppm' '$tmp/c.ppm'"

simule "$tmp/debut.ppm" corail --etapes 0
simule "$tmp/corail.ppm" corail --etapes 3000
simule "$tmp/mort.ppm" corail --k 0.07 --etapes 3000
resultat "au départ, quelques taches seulement (< 5 %)"  entre "$tmp/debut.ppm" 1 50
resultat "le corail envahit la grille (> 50 % en 3000 pas)" entre "$tmp/corail.ppm" 500 1000
resultat "k = 0,07 : tout le V disparaît"                entre "$tmp/mort.ppm" 0 0

if command -v magick > /dev/null; then
  simule "$tmp/corail.png" corail --etapes 3000
  magick "$tmp/corail.png" "$tmp/relu.ppm" 2> /dev/null
  resultat "le PNG fait main se relit pixel pour pixel (magick)" cmp <(tail -n +4 "$tmp/corail.ppm") <(tail -n +4 "$tmp/relu.ppm")
fi

resultat "argument inconnu : refusé"                     sh -c '! ./rd pas-un-motif'
resultat "sans terminal ni --export : refusé"            sh -c '! ./rd < /dev/null'

echo "$ok / $total"
[ "$ok" = "$total" ]

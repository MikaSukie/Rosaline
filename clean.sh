find . -name ".DS_Store" -type f -delete
find . -name "._*" -type f -delete
find . -name "__MACOSX" -type d -prune -exec rm -rf {} +

make -f DARMakeFile clean
git status --short

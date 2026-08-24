Commit 1:
- Changed some bugs in taker.py : line 101 conditional branch is identical, second condition should subtract
- Line 85 request sent without sender code from env
- Remove hardcoded fill on line 96


Commit 2:
- Added all quoter logic for all three instruments
- Added appropriate libraries compilers for C and CPP in docker
- Working dockerfile for quoter that takes in the --strategy tag
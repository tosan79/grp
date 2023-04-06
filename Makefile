CPP = g++
FLAGS = -std=c++17 -Wall -Wextra -pthread

grp: main.cpp
	$(CPP) main.cpp -o grp $(FLAGS)

make clean:
	rm grp grp.log grp.txt

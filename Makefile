main: src/main.c
	cc src/main.c -o main -Wall -Wextra -pedantic -Werror -O2

run: main
	@cat ./tests/001-simple-file-without-formulas.csv | ./main 
hot:
	@ls src/*.c | entr -c make run

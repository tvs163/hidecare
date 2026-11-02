SRC=../../src/cpp/frontend/native/app/main
test: test_vitals_core
	./test_vitals_core
test_vitals_core: test_vitals_core.c $(SRC)/vitals_core.c
	cc -O2 -Wall -Wextra -fsanitize=address,undefined -I$(SRC) -o $@ $^ -lm
clean:
	rm -f test_vitals_core

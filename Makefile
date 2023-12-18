build: wq-affinity

clean:
	@rm wq-affinity
	@rm *.bin

.PHONY: clean

%: src/%.c
	@$(CC) $(CCFLAGS) -Wall -O2 -D_GNU_SOURCE -luring -o $@ $<

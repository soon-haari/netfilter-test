netfilter-test: netfilter-test.cpp
	g++ -o netfilter-test netfilter-test.cpp -lnetfilter_queue

clean:
	rm -f netfilter-test

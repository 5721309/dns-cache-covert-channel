# Description

This program is written in POSIX environment.
It uses caching DNS server as communication medium
for establishing a covert channel.

Principle in short: when sender wants to send 1,
he caches next domain name in a list generated
with PRG (not cryptographically strong) by sending
request to resolve it. When response is not cached,
the time it takes to complete `getaddrinfo()` call
is greater than in the case when it is cached.
So receiver also sends request to resolve the same
address (he put same seed in PRG) and he finds out
that his request resolves fast enough to distinguish
given time (1) from uncached response time (0). 

Note that this channel has low bandwidth because
to read each bit in a message receiver sends
one dns request.
If each uncached request is processed in $70 \textup{ ms}$
and each cached is processed in $7 \textup{ ms}$,
then to receive a message of size $1024$ bits having half of ones
you have to wait $512 \cdot 70 + 512 \cdot 7 \textup{ (ms)} = 39427 \textup{ ms} \approx 39 \textup{ s}$
each bit in a message is at least one dns request
and one dns response.

# Minimal example

This example establishes covert channel with default parameters
that may not be optimal on every network.

Sender (must run first):
```bash
    echo "hello from the other side :)" | ./a.out -s 333
```

Receiver (must be run after sender has started but before cache expires):
```bash
    ./a.out -r 333
```


# Compilation

```sh
gcc main.c -lm
```

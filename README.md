PREFORK
=======

Quick Description
-----------------

prefork is a utility to prefork inetd-style wait services which
itself runs as an inetd-style wait service. prefork expects to be
passed file descriptor zero (0) as a listening socket on which
accept(2) can be called and will spawn children, passing them file
descriptor zero (0) under the expectation that these children will
service the incoming connexions. The children should then service
connexions by calling accept(2) on the socket.

For more documentation, please refer to the man page
([HTML](https://oskt.secure-endpoints.com/prefork.8.html),
 [PDF](https://oskt.secure-endpoints.com/prefork.8.pdf)).

Download
--------

For now, we have no official release but the source can be found
via git.

```
        $ git clone https://github.com/elric1/prefork
        $ cd prefork
        $ make
	$ make install
```

Building
--------

To build, just use make.

You can also build a debian package the definition of which is included
in the git repo.

Authors
-------

Roland C. Dowdeswell.

License
-------

Prefork is provided under a BSD/MIT style license.

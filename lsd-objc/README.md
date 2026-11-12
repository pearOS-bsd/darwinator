# lsd-objc – the LaunchServices database services, as a program of ours

`com.apple.lsd.mapdb` (`_LSDReadProtocol`) and `com.apple.lsd.modifydb` (`_LSDModifyProtocol`) served by an NSXPCListener.
It is a client of Foundation only: the protocols and the classes it needs (`FSNode`, `_CSStore`) are found by name at run
time, the calls are answered by hand.  Build it on a Mac (`clang -fobjc-arc -framework Foundation lsd.m -o lsd-objc`),
run it on pearOS next to the other stubs (tools/stack-dock-ownlsd.sh).

What it does now
- every call of the two protocols is logged with the signature of its reply block (`_Block_signature`), which is how the
  signatures were found: `getServerStoreNonBlocking...` replies `(_CSStore*, FSNode*, NSXPCListenerEndpoint*, BOOL, NSError*)`;
- `getServerStoreNonBlocking...` answers with a `_CSStore` made from the csstore file (`initWithContentsOfURL:error:`, read
  only), the `FSNode` of the file and flag 0 -- the answer the lsd of the Mac gives (found with a client program).  Dock takes
  it (without it it asks again and again);
- other calls with a reply block of one or two objects get nil.

The store file: `/var/db/lsd-store/store.csstore` (or `$LSD_STORE`).  For now it is a copy of the database that a Mac made
for its own applications -- a file of the VM only, never in the repository or in the ISO.  The next step is to write our
own csstore (the format is Apple's `_CSStore`: tables of arrays/strings/dictionaries, see the `_store` ivar) from the
bundles that exist on pearOS.

Notes
- NSInvocation on a reply block crashed (the block has no selector slot); the blocks are called by cast.
- Dock with this store still shows only empty slots (31 layers): the next thing to find is what makes it build the tiles.

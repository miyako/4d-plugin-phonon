//%attributes = {"invisible":true,"preemptive":"capable"}
var $info : Object
$info:=Phonon Get info

ASSERT($info.version#"")
ASSERT($info.engine="0.7.0")
ASSERT($info.threads>0)
ASSERT(Value type($info.models)=Is real)
ASSERT(Value type($info.speeches)=Is real)

// a ref that does not exist
ASSERT(Phonon Get status(-1).state=Phonon state unknown)
ASSERT(Phonon Get last error#"")

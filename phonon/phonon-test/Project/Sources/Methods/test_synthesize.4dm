//%attributes = {"invisible":true,"preemptive":"capable"}
var $model : Integer
$model:=Phonon Load model(phonon_test_model.path; {})
ASSERT($model>0; Phonon Get last error)

var $wav : Blob
$wav:=Phonon Synthesize($model; "Hello from Phonon."; {})
ASSERT(Phonon Get last error=""; Phonon Get last error)
ASSERT(BLOB size($wav)>44)
var $offset : Integer
ASSERT(BLOB to text($wav; UTF8 text without length; $offset; 4)="RIFF")

// 16-bit mono at 24 kHz: 48000 bytes per second
var $seconds : Real
$seconds:=(BLOB size($wav)-44)/48000
ASSERT(($seconds>0.5) && ($seconds<10))

/*
	A seed picks the take. It is reproduced bit for bit only on one thread
	(Phonon SET THREADS(1) before the first model is loaded): several
	threads sum in a varying order.
*/
var $a; $b : Blob
$a:=Phonon Synthesize($model; "Same text."; {seed: 7})
$b:=Phonon Synthesize($model; "Same text."; {seed: 8})
ASSERT(BLOB size($a)>44)
ASSERT(BLOB size($b)>44)

var $info : Object
var $threads : Integer
$info:=Phonon Get info
$threads:=$info.threads
ASSERT($threads>0)
Phonon SET THREADS(2)
$info:=Phonon Get info
ASSERT($info.threads=2)
Phonon SET THREADS($threads)
Phonon SET THREADS(-1)
ASSERT(Phonon Get last error#"")

// bad settings
$wav:=Phonon Synthesize($model; "Hello."; {voice: "nobody"})
ASSERT(BLOB size($wav)=0)
ASSERT(Phonon Get last error="@nobody@"; Phonon Get last error)
$wav:=Phonon Synthesize($model; "Hello."; {temperature: -1})
ASSERT(BLOB size($wav)=0)
ASSERT(Phonon Get last error#"")
$wav:=Phonon Synthesize(-1; "Hello."; {})
ASSERT(BLOB size($wav)=0)

Phonon UNLOAD MODEL($model)

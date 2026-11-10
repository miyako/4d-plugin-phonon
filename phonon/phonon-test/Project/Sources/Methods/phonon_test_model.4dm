//%attributes = {"invisible":true,"preemptive":"capable"}
#DECLARE() : 4D.Folder

/*
	The Phonon 31m English model (Apache 2.0), pinned to one revision of
	https://huggingface.co/gradium/phonon-31m-en and downloaded on first use.
*/

var $folder : 4D.Folder
$folder:=Folder(fk database folder).folder("models/phonon-31m-en")

var $revision : Text
$revision:="92741c5b07d085ff840e26fea8ce43110b87852f"

If (Not($folder.exists))
	$folder.create()
End if 

HTTP SET OPTION(HTTP follow redirect; 1)
HTTP SET OPTION(HTTP timeout; 600)

var $name : Text
For each ($name; ["config.json"; "tokenizer.json"; "model.q8.gguf"; "default-voice.safetensors"])
	If (Not($folder.file($name).exists))
		var $data : Blob
		var $status : Integer
		$status:=HTTP Request(HTTP GET method; "https://huggingface.co/gradium/phonon-31m-en/resolve/"+$revision+"/"+$name; ""; $data)
		ASSERT($status=200; "download failed: "+$name)
		$folder.file($name).setContent($data)
	End if 
End for each 

// a folder made from its platform path has a real POSIX .path, not /PACKAGE/...
return Folder($folder.platformPath; fk platform path)

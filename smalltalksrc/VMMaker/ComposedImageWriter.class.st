Class {
	#name : 'ComposedImageWriter',
	#superclass : 'AbstractComposedImageAccess',
	#traits : 'TImageWriter',
	#classTraits : 'TImageWriter classTrait',
	#category : 'VMMaker-ImageFormat',
	#package : 'VMMaker',
	#tag : 'ImageFormat'
}

{ #category : 'translation' }
ComposedImageWriter class >> declareCVarsIn: aCCodeGenerator [
	"currently Slang lacks some support for subclasses: prevent <sys/stat.h> to be include a second time during C code generation as the superclass already includes it but declareCVarsIn: is called for each concrete class logging the warning inside addHeaderFile: method"
]

{ #category : 'perm - space' }
ComposedImageWriter >> ensureDeletePermanentSpaceData: imageFileName [

	"Delete the perm space data file of the image before writing a new one, instead of truncating it.
	 VMs started on this image may map that file, this VM too: Windows refuses to truncate a mapped
	 file (ERROR_USER_MAPPED_FILE), and on Unix truncating it changes the pages that their MAP_PRIVATE
	 views have not copied yet. A deleted file stays alive, unchanged, for the processes that map it."

	<inline: false>
	<var: #fileName type: #'char *'>
	<var: #file type: #sqImageFile>
	| fileName file |
	"A statement of its own: permSpaceDataFileInImage: is inlined and does not translate as an argument."
	fileName := self permSpaceDataFileInImage: imageFileName.
	"Not fileExists: first: it uses stat, which fails on Windows for files of 2 GB or more.
	 If the delete fails, the file is in the way only if it is there."
	(self unlink: fileName) = 0 ifFalse: [
		"In simulation unlink: fails only when the file is not there."
		file := self cCode: [ self sqImageFile: fileName Open: 'rb' ] inSmalltalk: [ nil ].
		file ifNotNil: [
			self sqImageFileClose: file.
			self logError: 'Error deleting %s' _: fileName.
			coInterpreter primitiveFail ] ]
]

{ #category : 'writing' }
ComposedImageWriter >> ensureDeleteSegments: imageFileName [ 
	
	| segmentIndex r |
	segmentIndex := 0.
	
	[ self existSegment: segmentIndex inImage: imageFileName ] 
		whileTrue: [ 
			r := self unlink: (self segmentMetadataFile: segmentIndex inImage: imageFileName). 
			r ~= 0 ifTrue: [ 
					self logError: 'Error deleting %s' _: (self segmentMetadataFile: segmentIndex inImage: imageFileName).
					^ coInterpreter primitiveFail. ].
			
			r := self unlink: (self segmentDataFile: segmentIndex inImage: imageFileName). 
			r ~= 0 ifTrue: [ 
					self logError: 'Error deleting %s' _: (self segmentDataFile: segmentIndex inImage: imageFileName).
					^ coInterpreter primitiveFail. ].
			segmentIndex := segmentIndex + 1 ]
]

{ #category : 'writing' }
ComposedImageWriter >> unlink: aFileName [

	<doNotGenerate>
	| fileReference |
	
	fileReference := aFileName asFileReference.
	fileReference exists ifFalse: [ ^ -1 ].
	fileReference delete.
	
	^ 0
]

{ #category : 'writing' }
ComposedImageWriter >> writeDataStatingAt: start withSize: size inFileName: fullFileName [

	<var: #dataFile type: #sqImageFile>
	<var: #nWritten type: #'size_t'>
	<returnTypeC: #'size_t'>
	
	| dataFile nWritten |
	dataFile := self sqImageFile: fullFileName Open: 'wb'.
	dataFile ifNil: [ "could not open the file for writing" 
		^ self primitiveFail ].

	nWritten := 0.

	size > 0 ifTrue:[	
			nWritten := self
		            sq: start
		            Image: 1
		            File: size
		            Write: dataFile].

	self sqImageFileClose: dataFile.

	^ nWritten
]

{ #category : 'writing' }
ComposedImageWriter >> writeHeaderFile: imageFileName fromHeader: header [

	<inline: true>
	<var: #file type: #sqImageFile>
	<var: #buffer declareC: 'char buffer[255]'>

	| buffer file |
		
	self simulationOnly: [ buffer := nil ].
	
	file := self sqImageFile: (self headerFileNameinImage: imageFileName into: buffer bufferSize: 255) Open: 'w'.
	file ifNil: [ "could not open the file for writing" 
		^ self primitiveFail ].

	self writeHeaderSTON: header toFile: file.

	self sqImageFileClose: file.

]

{ #category : 'writing' }
ComposedImageWriter >> writeHeaderSTON: header toFile: f [ 

	self writeSTON: header toFile: f
]

{ #category : 'api' }
ComposedImageWriter >> writeImageFile: imageFileName fromHeader: header [

	"Write one image header file + many segments files (metadata & data per segment)"

	<inline: false>

	<var: #imageFileName declareC: 'char *imageFileName'>
	<var: #oldSpaceBytesWritten type:#'size_t'>
	<var: #permanentSpaceBytesWritten type:#'size_t'>
	<var: #bytesWritten type:#'size_t'>
	<var: #header type: #SpurImageHeaderStruct> "TODO: Rename this class"

	| oldSpaceBytesWritten permanentSpaceBytesWritten bytesWritten |

	self logDebug: 'Writing snapshot %s' _: imageFileName.

	self createImageDirectory: imageFileName.

	"Before anything is written: if the old perm data cannot be deleted, the image stays as it was."
	self ensureDeletePermanentSpaceData: imageFileName.
	coInterpreter failed ifTrue: [ ^ self ].

	self writeHeaderFile: imageFileName fromHeader: header.

	self flag: #TODO. "success: header imageHeaderSize?"

	oldSpaceBytesWritten := self writeImageSegments: imageFileName.
	
	permanentSpaceBytesWritten := self writeImagePermanentSpace: imageFileName.

	bytesWritten := oldSpaceBytesWritten + permanentSpaceBytesWritten.

	coInterpreter success: bytesWritten = header dataSize 
]

{ #category : 'perm - space' }
ComposedImageWriter >> writeImagePermanentSpace: imageFileName [

	<returnTypeC: #'size_t'>
	
	self writePermanentSpaceMetadata: imageFileName.

	^ self writePermanentSpaceData: imageFileName
]

{ #category : 'segments' }
ComposedImageWriter >> writeImageSegments: imageFileName [

	<var: 'aSegment' type:#'SpurSegmentInfo *'>
	<returnTypeC: #'size_t'>

	self beginWriteImageSegments.
	self ensureDeleteSegments: imageFileName.
	
	coInterpreter failed ifTrue: [ ^ 0 ].

	^ segmentManager withNonEmptySegmentsDo: [ :aSegment :segmentIndex | 
		self
          writeSegment: aSegment
          nextIndex: segmentIndex
			 inImage: imageFileName ].


]

{ #category : 'writing' }
ComposedImageWriter >> writeMetadata: metadata inFileName: fullFileName [

	<var: #metadataFile type: #sqImageFile>
	<var: #metadata type: #ComposedMetadataStruct>

	| metadataFile |

	metadataFile := self sqImageFile: fullFileName Open: 'w'.
	metadataFile ifNil: [ "could not open the file for writing" 
		^ self primitiveFail ].

	self writeSTON: metadata toFile: metadataFile.

	self sqImageFileClose: metadataFile.


]

{ #category : 'perm - space' }
ComposedImageWriter >> writePermanentSpaceData: imageFileName [

	<returnTypeC: #'size_t'>

	^ self
		  writeDataStatingAt: objectMemory getMemoryMap permSpaceStart asVoidPointer
		  withSize: objectMemory totalBytesInPermanentSpace
		  inFileName: (self permSpaceDataFileInImage: imageFileName)
]

{ #category : 'perm - space' }
ComposedImageWriter >> writePermanentSpaceMetadata: imageFileName [

	<var: #metadata type: #ComposedMetadataStruct>
	| metadata |
	self simulationOnly: [ metadata := ComposedMetadataStruct new ].
	metadata startAddress: objectMemory getMemoryMap permSpaceStart.
	metadata dataSize: objectMemory totalBytesInPermanentSpace.

	self
		writeMetadata: metadata
		inFileName: (self permSpaceMetadataFileNameInImage: imageFileName)
]

{ #category : 'writing' }
ComposedImageWriter >> writeSTON: struct toFile: f [

	<inline: true>
	struct withStructNameDo: [ :name | self fprintf: f _: self headFormat _: name ].
	struct withFieldsDo: [ :fieldName :fieldValue |
			self fprintf: f _: self fieldFormat _: fieldName _: (self cCoerce: fieldValue to: #sqInt)]
		separatedBy: [ self fprintf: f _: ',\n'].
		
	self fprintf: f _: '\n}'. 
]

{ #category : 'segments' }
ComposedImageWriter >> writeSegment: segment nextIndex: segmentIndex inImage: imageFileName [

	<inline: false>
	<var: 'segment' type: #'SpurSegmentInfo *'>
	<var: #imageFileName declareC: 'char *imageFileName'>
	<returnTypeC: #'size_t'>

	self logDebug: (self msgFormat: 'Segment Start: ') _: segment segStart.
	self logDebug: (self msgFormat: 'Segment Size: ') _: segment segSize.

	self writeSegmentMetadata: segment nextIndex: segmentIndex inImage: imageFileName.

	^ self writeSegmentData: segment nextIndex: segmentIndex inImage: imageFileName.
]

{ #category : 'segments' }
ComposedImageWriter >> writeSegmentData: segment nextIndex: segmentIndex inImage: imageFileName [

	<returnTypeC: #'size_t'>

	^ self
		  writeDataStatingAt: segment segStart asVoidPointer
		  withSize: segment segSize
		  inFileName: (self segmentDataFile: segmentIndex inImage: imageFileName)
]

{ #category : 'segments' }
ComposedImageWriter >> writeSegmentMetadata: segment nextIndex: segmentIndex inImage: imageFileName [

	<var: #metadata type: #ComposedMetadataStruct>
	| metadata |

	self simulationOnly: [ metadata := ComposedMetadataStruct new. ].
	metadata startAddress: segment segStart.
	metadata dataSize: segment segSize.

	self
		writeMetadata: metadata
		inFileName: (self segmentMetadataFile: segmentIndex inImage: imageFileName)
]

{ #category : 'writing' }
ComposedImageWriter >> writeSegmentSTON: segmentMetadata toFile: f [ 

	<var: #f type: #sqImageFile>
	<var: #segmentMetadata type: #ComposedMetadataStruct>

	self writeSTON: segmentMetadata toFile: f
]

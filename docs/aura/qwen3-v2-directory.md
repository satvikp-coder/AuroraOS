# AURQ3V2 tensor directory

The V2 wire format describes row-major signed int8 tensors for a grouped-query Qwen3 model. All integers are little-endian. Metadata readers can use a 56-byte buffer for either version. V1 retains its 48-byte header and existing directory rules.

The V2 header begins with the eight bytes `AURQ3V2\0`. At byte 8, eight consecutive uint32 fields hold version (2), vocabulary size, hidden size, intermediate size, layer count, query-head count, head dimension, and tensor count. A uint64 at byte 40 gives the exact object size. The uint32 at byte 48 gives the KV-head count, which must divide the query-head count. The uint32 at byte 52 holds flags. Bit 0 selects tied embeddings. All other bits must be zero.

Each 32-byte descriptor contains the layer and role (uint16 each), rows, columns and shift (uint32 each), followed by absolute offset and byte count (uint64 each). Shifts range from 0 through 31. The directory immediately follows the header. Its payloads appear consecutively in directory order and fill the object. Call `q3m_validate_v2_directory` after parsing the header to preflight every descriptor and its extent. This operation reads only metadata.

The first two entries are the embedding matrix (vocabulary by hidden) and final RMSNorm (1 by hidden). Untied models add a third entry for the output matrix (vocabulary by hidden). Global entries use layer 65535. Tied models compute logits by multiplying the same embedding matrix by the final hidden vector. This format stores it once. A converter processing a checkpoint containing both embedding and output tensors must verify equality before choosing the tied representation.

Each layer then contributes exactly eleven entries in the following order. Q and KV widths are their respective head counts times the head dimension.

| Role | Rows | Columns |
| --- | --- | --- |
| Input RMSNorm | 1 | Hidden |
| Query projection | Q width | Hidden |
| Key projection | KV width | Hidden |
| Value projection | KV width | Hidden |
| Query RMSNorm | 1 | Head dimension |
| Key RMSNorm | 1 | Head dimension |
| Attention output projection | Hidden | Q width |
| Post-attention RMSNorm | 1 | Hidden |
| Gate projection | Intermediate | Hidden |
| Up projection | Intermediate | Hidden |
| Down projection | Hidden | Intermediate |

For Qwen3-0.6B, the tied directory has 310 entries across 28 layers. The query width is 2048, the KV width is 1024 and the residual hidden size is 1024. This contract describes tensor geometry and storage. Model conversion, quantization calibration and decoding require additional implementation.

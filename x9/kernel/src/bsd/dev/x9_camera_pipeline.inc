/* MT6993 receive-plan builder. No MMIO, allocation, power or stream-on.
 * Captured board graph: hhfront endpoint <6ef> -> CSI port0 <708>,
 * hhmain endpoint <6f2> -> CSI port3 <71a>. Not sensor index == CSI index.
 * ISP8s phy_3_0 outmux0..5 connect to CAMSV, 6..8 to RAW, 9 to UISP.
 * Candidate outmux0/CAMSV0 is NOT reserved merely by returning this plan.
 *
 * Numeric fields follow the currently supplied OPPO phy_3_0 outmux and
 * mtk_cam-sv-regs.h descriptions. The old unit-test TG interface is not
 * used. Completion must be CAMSV SW_GP_PASS1_DONE with matching group/tag
 * and frame sequence, not CQ setting-done or SENINF's input EOF.
 */
_Static_assert(sizeof(struct x9_camera_pipeline)==128,"camera pipeline ABI");
static int cam_pipeline_plan(uint32_t which, struct x9_camera_pipeline *p)
{
    if(which!=X9_CAMERA_BACK && which!=X9_CAMERA_FRONT)return EINVAL;
    const struct x9_camera_sensor *s=&sensors[which-1];
    if(!s->width || s->width%4 || !s->height || s->width>65535 || s->height>65535)return EINVAL;
    uint32_t stride=(s->width/4*5+63)&~63u;
    uint64_t bytes=(uint64_t)stride*s->height;
    if(stride>65535 || bytes>X9_CAMERA_MAX_BYTES)return EMSGSIZE;
    memset(p,0,sizeof(*p));p->version=1;p->sensor=which;
    p->flags=X9_CAM_PIPE_SOURCE_PLAN;
    p->blockers=X9_CAM_PIPE_NEED_ID|X9_CAM_PIPE_NEED_MM_POWER|X9_CAM_PIPE_NEED_PHY|
        X9_CAM_PIPE_NEED_ROUTE|X9_CAM_PIPE_NEED_DMA_MAP|X9_CAM_PIPE_NEED_DRAIN;
    if(st.probe_mask&(1u<<which)) {
        p->flags|=X9_CAM_PIPE_SENSOR_PROBED;p->blockers&=~X9_CAM_PIPE_NEED_ID;
    }
    p->csi_port=which==X9_CAMERA_BACK?3:0;p->async=p->csi_port;
    p->cphy=s->cphy;p->lanes=s->lanes;
    p->width=s->width;p->height=s->height;p->stride=stride;p->bytes=(uint32_t)bytes;
    p->vc=0;p->dt=0x2b;
    p->outmux_candidate=0;p->camsv_candidate=0;
    p->dma_bits=35;p->smmu_sid=0x80;
    /* Initial route config calls _config_outmux -> SET_TAG, not SET_TAG_V2:
     * HSIZE is packed CSI word-count BYTES minus1, VSIZE is lines minus1.
     * The later apply-v2 path has different HSIZE semantics. Do not infer
     * this register's units from the header or the name exp_hsize. */
    p->vcdt=(p->dt<<16)|(p->vc<<8)|1;
    p->expected_size=((p->height-1)<<16)|(p->width/4*5-1);
    /* CAMSV central tag1 grab +0x548/+0x54c, RAW10 packed +0x554.
     * DMA bank +0x210 has stride in HIGH16, max-burst in LOW5. */
    p->grab_pixels=p->width<<16;p->grab_lines=p->height<<16;
    p->format=1;p->dma_basic=(stride<<16)|0x10;
    p->csi_base=0x13880000ull+p->csi_port*0x8000ull;
    p->camsv_base=0x3a600000;p->smmu_base=0x30800000;
    /* DT mm_smmu_cam dma-range is 16GiB despite the 35-bit DMA mask.
     * Also obey the shared mm_common group's reservations when allocating.
     * CPU PA is never accepted as a camera IOVA by this plan interface. */
    p->dma_limit=0x400000000ull;
    return 0;
}

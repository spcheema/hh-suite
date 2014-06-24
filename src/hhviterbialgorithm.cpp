
#include "hhviterbi.h"
#include "hhviterbimatrix.h"


#define MAX2_SET_MASK(vec1, vec2, vec3, res)        \
res_gt_vec = (simd_int)simdf32_gt(vec1,vec2);      \
index_vec  = simdi_and(res_gt_vec,vec3);          \
res        = simdi_xor(res,index_vec);


#define MAX2(vec1, vec2, vec3, res)           \
res_gt_vec = (simd_int)simdf32_gt(vec1,vec2);          \
index_vec  = simdi_and(res_gt_vec,vec3);          \
res        = simdui8_max(res,index_vec);




/////////////////////////////////////////////////////////////////////////////////////
// Compare HMMs with one another and look for sub-optimal alignments that share no pair with previous ones
// The function is called with q and t
/////////////////////////////////////////////////////////////////////////////////////
#ifdef VITERBI_CELLOFF
void Viterbi::AlignWithCellOff(HMMSimd* q, HMMSimd* t,ViterbiMatrix * viterbiMatrix, int maxres, ViterbiResult* result)
#else
void Viterbi::AlignWithOutCellOff(HMMSimd* q, HMMSimd* t,ViterbiMatrix * viterbiMatrix, int maxres, ViterbiResult* result)
#endif
{
    
    // Linear topology of query (and template) HMM:
    // 1. The HMM HMM has L+2 columns. Columns 1 to L contain
    //    a match state, a delete state and an insert state each.
    // 2. The Start state is M0, the virtual match state in column i=0 (j=0). (Therefore X[k][0]=ANY)
    //    This column has only a match state and it has only a transitions to the next match state.
    // 3. The End state is M(L+1), the virtual match state in column i=L+1.(j=L+1) (Therefore X[k][L+1]=ANY)
    //    Column L has no transitions to the delete state: tr[L][M2D]=tr[L][D2D]=0.
    // 4. Transitions I->D and D->I are ignored, since they do not appear in PsiBlast alignments
    //    (as long as the gap opening penalty d is higher than the best match score S(a,b)).
    
    // Pairwise alignment of two HMMs:
    // 1. Pair-states for the alignment of two HMMs are
    //    MM (Q:Match T:Match) , GD (Q:Gap T:Delete), IM (Q:Insert T:Match),  DG (Q:Delelte, T:Match) , MI (Q:Match T:Insert)
    // 2. Transitions are allowed only between the MM-state and each of the four other states.
    
    // Saving space:
    // The best score ending in pair state XY sXY[i][j] is calculated from left to right (j=1->t->L)
    // and top to bottom (i=1->q->L). To save space, only the last row of scores calculated is kept in memory.
    // (The backtracing matrices are kept entirely in memory [O(t->L*q->L)]).
    // When the calculation has proceeded up to the point where the scores for cell (i,j) are caculated,
    //    sXY[i-1][j'] = sXY[j']   for j'>=j (A below)
    //    sXY[i][j']   = sXY[j']   for j'<j  (B below)
    //    sXY[i-1][j-1]= sXY_i_1_j_1         (C below)
    //    sXY[i][j]    = sXY_i_j             (D below)
    //                   j-1
    //                     j
    // i-1:               CAAAAAAAAAAAAAAAAAA
    //  i :   BBBBBBBBBBBBBD
    // Variable declarations
    
    const float smin = (this->local ? 0 : -FLT_MAX);  //used to distinguish between SW and NW algorithms in maximization
    const simd_float smin_vec    = simdf32_set(smin);
    const simd_float shift_vec   = simdf32_set(shift);
//    const simd_float one_vec     = simdf32_set(1); //   00000001
    const simd_int mm_vec        = simdi32_set(2); //MM 00000010
    const simd_int gd_vec        = simdi32_set(3); //GD 00000011
    const simd_int im_vec        = simdi32_set(4); //IM 00000100
    const simd_int dg_vec        = simdi32_set(5); //DG 00000101
    const simd_int mi_vec        = simdi32_set(6); //MI 00000110
    const simd_int gd_mm_vec     = simdi32_set(8); //   00001000
    const simd_int im_mm_vec     = simdi32_set(16);//   00010000
    const simd_int dg_mm_vec     = simdi32_set(32);//   00100000
    const simd_int mi_mm_vec     = simdi32_set(64);//   01000000
    
    
#ifdef AVX2_SUPPORT
    const simd_int shuffle_mask_extract = _mm256_setr_epi8(0,  4,  8,  12, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
                                                           -1, -1, -1,  -1,  0,  4,  8, 12, -1, -1, -1, -1, -1, -1, -1, -1);
#endif
#ifdef VITERBI_CELLOFF
    const __m128i tmp_vec        = _mm_set_epi32(0x40000000,0x00400000,0x00004000,0x00000040);//01000000010000000100000001000000
#ifdef AVX2_SUPPORT
    const simd_int co_vec               = _mm256_inserti128_si256(_mm256_castsi128_si256(tmp_vec), tmp_vec, 1);
    const simd_int float_min_vec     = (simd_int) _mm256_set1_ps(-FLT_MAX);
    const simd_int shuffle_mask_celloff = _mm256_set_epi8(
                                                          15, 14, 13, 12,
                                                          15, 14, 13, 12,
                                                          15, 14, 13, 12,
                                                          15, 14, 13, 12,
                                                          3, 2,  1, 0,
                                                          3, 2,  1, 0,
                                                          3, 2,  1, 0,
                                                          3, 2,  1, 0);
#else // SSE case
    const simd_int co_vec = tmp_vec;
    const simd_int float_min_vec = (simd_int) simdf32_set(-FLT_MAX);
#endif
#endif // AVX2 end
    
    int i,j;      //query and template match state indices
    simd_int i2_vec = simdi32_set(0);
    simd_int j2_vec = simdi32_set(0);
    
    simd_float sMM_i_j = simdf32_set(0);
    simd_float sMI_i_j,sIM_i_j,sGD_i_j,sDG_i_j;
    
    
    simd_float Si_vec;
    simd_float sMM_i_1_j_1;
    simd_float sMI_i_1_j_1;
    simd_float sIM_i_1_j_1;
    simd_float sGD_i_1_j_1;
    simd_float sDG_i_1_j_1;
    
    simd_float score_vec     = simdf32_set(-FLT_MAX);
    simd_int byte_result_vec = simdi32_set(0);
    
    
    // Initialization of top row, i.e. cells (0,j)
    for (j=0; j <= t->L; ++j)
    {
        const unsigned int index_pos_j = j * 5;
        sMM_DG_MI_GD_IM_vec[index_pos_j + 0] = simdf32_set(-j*penalty_gap_template);
        sMM_DG_MI_GD_IM_vec[index_pos_j + 1] = simdf32_set(-FLT_MAX);
        sMM_DG_MI_GD_IM_vec[index_pos_j + 2] = simdf32_set(-FLT_MAX);
        sMM_DG_MI_GD_IM_vec[index_pos_j + 3] = simdf32_set(-FLT_MAX);
        sMM_DG_MI_GD_IM_vec[index_pos_j + 4] = simdf32_set(-FLT_MAX);

    }
    
    // Viterbi algorithm
    const int queryLength = q->L;
    for (i=1; i <= queryLength; ++i) // Loop through query positions i
    {
        
        // If q is compared to t, exclude regions where overlap of q with t < min_overlap residues
        // Initialize cells
        sMM_i_1_j_1 = simdf32_set(-(i-1)*penalty_gap_query);  // initialize at (i-1,0)
        sIM_i_1_j_1 = simdf32_set(-FLT_MAX); // initialize at (i-1,jmin-1)
        sMI_i_1_j_1 = simdf32_set(-FLT_MAX);
        sDG_i_1_j_1 = simdf32_set(-FLT_MAX);
        sGD_i_1_j_1 = simdf32_set(-FLT_MAX);
        
        // initialize at (i,jmin-1)
        const unsigned int index_pos_i = 0 * 5;
        sMM_DG_MI_GD_IM_vec[index_pos_i + 0] = simdf32_set(-i*penalty_gap_query);           // initialize at (i,0)
        sMM_DG_MI_GD_IM_vec[index_pos_i + 1] = simdf32_set(-FLT_MAX);
        sMM_DG_MI_GD_IM_vec[index_pos_i + 2] = simdf32_set(-FLT_MAX);
        sMM_DG_MI_GD_IM_vec[index_pos_i + 3] = simdf32_set(-FLT_MAX);
        sMM_DG_MI_GD_IM_vec[index_pos_i + 4] = simdf32_set(-FLT_MAX);
#ifdef AVX2_SUPPORT
        unsigned long long * sCO_MI_DG_IM_GD_MM_vec = (unsigned long long *) viterbiMatrix->getRow(i);
#else
        unsigned int   * sCO_MI_DG_IM_GD_MM_vec   = (unsigned int *) viterbiMatrix->getRow(i);
#endif
        
        const unsigned int start_pos_tr_i_1 = (i-1) * 7;
        const unsigned int start_pos_tr_i = (i) * 7;
        const simd_float q_m2m = simdf32_load((float *) (q->tr+start_pos_tr_i_1 + 2)); // M2M
        const simd_float q_m2d = simdf32_load((float *) (q->tr+start_pos_tr_i_1 + 3)); // M2D
        const simd_float q_d2m = simdf32_load((float *) (q->tr+start_pos_tr_i_1 + 4)); // D2M
        const simd_float q_d2d = simdf32_load((float *) (q->tr+start_pos_tr_i_1 + 5)); // D2D
        const simd_float q_i2m = simdf32_load((float *) (q->tr+start_pos_tr_i_1 + 6)); // I2m
        const simd_float q_i2i = simdf32_load((float *) (q->tr+start_pos_tr_i      )); // I2I
        const simd_float q_m2i = simdf32_load((float *) (q->tr+start_pos_tr_i   + 1)); // M2I

        
        // Find maximum score; global alignment: maxize only over last row and last column
        const bool findMaxInnerLoop = (local || i == queryLength);
        const int targetLength = t->L;
        for (j=1; j <= targetLength; ++j) // Loop through template positions j
        {
            simd_int index_vec;
            simd_int res_gt_vec;
            // cache line optimized reading
            const unsigned int start_pos_tr_j_1 = (j-1) * 7;
            const unsigned int start_pos_tr_j = (j) * 7;

            const simd_float t_m2m = simdf32_load((float *) (t->tr+start_pos_tr_j_1+2)); // M2M
            const simd_float t_m2d = simdf32_load((float *) (t->tr+start_pos_tr_j_1+3)); // M2D
            const simd_float t_d2m = simdf32_load((float *) (t->tr+start_pos_tr_j_1+4)); // D2M
            const simd_float t_d2d = simdf32_load((float *) (t->tr+start_pos_tr_j_1+5)); // D2D
            const simd_float t_i2m = simdf32_load((float *) (t->tr+start_pos_tr_j_1+6)); // I2m
            const simd_float t_i2i = simdf32_load((float *) (t->tr+start_pos_tr_j));   // I2i
            const simd_float t_m2i = simdf32_load((float *) (t->tr+start_pos_tr_j+1));     // M2I
           
            // mm < min { 0 }
            byte_result_vec = simdi_xor(byte_result_vec, byte_result_vec); // set 0 (faster)
            // Find max value
            // CALCULATE_MAX6( sMM_i_j,
            //                 smin,
            //                 sMM_i_1_j_1 + q->tr[i-1][M2M] + t->tr[j-1][M2M],
            //                 sGD_i_1_j_1 + q->tr[i-1][M2M] + t->tr[j-1][D2M],
            //                 sIM_i_1_j_1 + q->tr[i-1][I2M] + t->tr[j-1][M2M],
            //                 sDG_i_1_j_1 + q->tr[i-1][D2M] + t->tr[j-1][M2M],
            //                 sMI_i_1_j_1 + q->tr[i-1][M2M] + t->tr[j-1][I2M],
            //                 bMM[i][j]
            //                 );
            // same as sMM_i_1_j_1 + q->tr[i-1][M2M] + t->tr[j-1][M2M]
            simd_float mm_m2m_m2m_vec = simdf32_add( simdf32_add(sMM_i_1_j_1, q_m2m), t_m2m);
            simd_float gd_m2m_d2m_vec = simdf32_add( simdf32_add(sGD_i_1_j_1, q_m2m), t_d2m);
            simd_float im_m2m_d2m_vec = simdf32_add( simdf32_add(sIM_i_1_j_1, q_i2m), t_m2m);
            simd_float dg_m2m_d2m_vec = simdf32_add( simdf32_add(sDG_i_1_j_1, q_d2m), t_m2m);
            simd_float mi_m2m_d2m_vec = simdf32_add( simdf32_add(sMI_i_1_j_1, q_m2m), t_i2m);

            const simd_float max1 = simdf32_max(smin_vec, mm_m2m_m2m_vec);
            const simd_float max2 = simdf32_max(gd_m2m_d2m_vec, im_m2m_d2m_vec);
            const simd_float max3 = simdf32_max(dg_m2m_d2m_vec, mi_m2m_d2m_vec);
            const simd_float max4 = simdf32_max(max1, max2); // smin mm gd im
            sMM_i_j = simdf32_max(max4, max3);  // smin mm gd im dg mi

            simd_int res1 = simdf_f2icast(simdf32_eq(mm_m2m_m2m_vec, sMM_i_j)); // 0 1 0 0
            res1 = simdi_and(mm_vec, res1);
            simd_int res3 = simdf_f2icast(simdf32_eq(im_m2m_d2m_vec, sMM_i_j)); // 0 0 1 0
            res3 = simdi_and(im_vec, res3);
            simd_int res2 = simdf_f2icast(simdf32_eq(gd_m2m_d2m_vec, sMM_i_j)); // 1 0 0 0
            res2 = simdi_and(gd_vec, res2);
            simd_int res4 = simdf_f2icast(simdf32_eq(dg_m2m_d2m_vec, sMM_i_j)); // 0 0 0 0
            res4 = simdi_and(dg_vec, res4);
            simd_int res5 = simdf_f2icast(simdf32_eq(mi_m2m_d2m_vec, sMM_i_j)); // 0 0 0 0
            res5 = simdi_and(mi_vec, res5);
            
            res1 = simdui8_max(res1,res2);
            res3 = simdui8_max(res3,res4);
            res5 = simdui8_max(res1,res5);
            res4 = simdui8_max(res5,res3);
            byte_result_vec =  simdui8_max(res4, byte_result_vec);
            
            
            //for(int i = 0; i < 4; i++){
            //    std::cout << ((float *) &sMM_i_j )[i] << " ";
            //    std::cout << ((float *) &sMM_i_j_new)[i] << " ";
            //}
           // std::cout << std::endl;
            
//            for(int i = 0; i < 4; i++){
//                if(((int *) &byte_result_vec )[i] != ((int *) &byte_result_vec_new)[i]){
//                    std::cout << ((float *) &smin_vec )[i] << " ";
//                    std::cout << ((float *) &mm_m2m_m2m_vec )[i] << " ";
//                    std::cout << ((float *) &gd_m2m_d2m_vec )[i] << " ";
//                    std::cout << ((float *) &im_m2m_d2m_vec )[i] << " ";
//                    std::cout << ((float *) &dg_m2m_d2m_vec )[i] << " ";
//                    std::cout << ((float *) &mi_m2m_d2m_vec )[i] << " ";
//
//                    
//                    std::cout << ((float *) &sMM_i_j )[i] << " ";
//                    std::cout << ((float *) &sMM_i_j_new)[i] << " ";
//                    std::cout << ((int *) &byte_result_vec )[i] << " ";
//                    std::cout << ((int *) &byte_result_vec_new)[i] << " ";
//                    std::cout << std::endl;
//
//                }
//            }
         //   std::cout << std::endl;
            // TODO add secondary structure score
            // calculate amino acid profile-profile scores
            Si_vec = log2f4(ScalarProd20Vec((simd_float *) q->p[i],(simd_float *) t->p[j]));
            Si_vec = simdf32_add(Si_vec, shift_vec);
            
            sMM_i_j = simdf32_add(sMM_i_j, Si_vec);
            //+ ScoreSS(q,t,i,j) + shift + (Sstruc==NULL? 0: Sstruc[i][j]);
            
            const unsigned int index_pos_j   = (j * 5);
            const unsigned int index_pos_j_1 = (j - 1) * 5;
            const simd_float sMM_j_1 = simdf32_load((float *) (sMM_DG_MI_GD_IM_vec + index_pos_j_1 + 0));
            const simd_float sGD_j_1 = simdf32_load((float *) (sMM_DG_MI_GD_IM_vec + index_pos_j_1 + 3));
            const simd_float sIM_j_1 = simdf32_load((float *) (sMM_DG_MI_GD_IM_vec + index_pos_j_1 + 4));
            const simd_float sMM_j   = simdf32_load((float *) (sMM_DG_MI_GD_IM_vec + index_pos_j + 0));
            const simd_float sDG_j   = simdf32_load((float *) (sMM_DG_MI_GD_IM_vec + index_pos_j + 1));
            const simd_float sMI_j   = simdf32_load((float *) (sMM_DG_MI_GD_IM_vec + index_pos_j + 2));
            sMM_i_1_j_1 = simdf32_load((float *)(sMM_DG_MI_GD_IM_vec + index_pos_j + 0));
            sDG_i_1_j_1 = simdf32_load((float *)(sMM_DG_MI_GD_IM_vec + index_pos_j + 1));
            sMI_i_1_j_1 = simdf32_load((float *)(sMM_DG_MI_GD_IM_vec + index_pos_j + 2));
            sGD_i_1_j_1 = simdf32_load((float *)(sMM_DG_MI_GD_IM_vec + index_pos_j + 3));
            sIM_i_1_j_1 = simdf32_load((float *)(sMM_DG_MI_GD_IM_vec + index_pos_j + 4));
            
            //            sGD_i_j = max2
            //            (
            //             sMM[j-1] + t->tr[j-1][M2D], // MM->GD gap opening in query
            //             sGD[j-1] + t->tr[j-1][D2D], // GD->GD gap extension in query
            //             bGD[i][j]
            //             );
            //sMM_DG_GD_MI_IM_vec
            simd_float mm_gd_vec = simdf32_add(sMM_j_1, t_m2d); // MM->GD gap opening in query
            simd_float gd_gd_vec = simdf32_add(sGD_j_1, t_d2d); // GD->GD gap extension in query
            // if mm_gd > gd_dg { 8 }
            MAX2_SET_MASK(mm_gd_vec, gd_gd_vec,gd_mm_vec, byte_result_vec);
            
            sGD_i_j = simdf32_max(
                                 mm_gd_vec,
                                 gd_gd_vec
                                 );
            //            sIM_i_j = max2
            //            (
            //             sMM[j-1] + q->tr[i][M2I] + t->tr[j-1][M2M] ,
            //             sIM[j-1] + q->tr[i][I2I] + t->tr[j-1][M2M], // IM->IM gap extension in query
            //             bIM[i][j]
            //             );
            
            
            simd_float mm_mm_vec = simdf32_add(simdf32_add(sMM_j_1, q_m2i), t_m2m);
            simd_float im_im_vec = simdf32_add(simdf32_add(sIM_j_1, q_i2i), t_m2m); // IM->IM gap extension in query
            // if mm_mm > im_im { 16 }
            MAX2_SET_MASK(mm_mm_vec,im_im_vec, im_mm_vec, byte_result_vec);
            
            sIM_i_j = simdf32_max(
                                  mm_mm_vec,
                                  im_im_vec
                                  );
            
            //            sDG_i_j = max2
            //            (
            //             sMM[j] + q->tr[i-1][M2D],
            //             sDG[j] + q->tr[i-1][D2D], //gap extension (DD) in query
            //             bDG[i][j]
            //             );
            simd_float mm_dg_vec = simdf32_add(sMM_j, q_m2d);
            simd_float dg_dg_vec = simdf32_add(sDG_j, q_d2d); //gap extension (DD) in query
            // if mm_dg > dg_dg { 32 }
            MAX2_SET_MASK(mm_dg_vec,dg_dg_vec, dg_mm_vec, byte_result_vec);
            
            sDG_i_j = simdf32_max( mm_dg_vec
                                  ,
                                  dg_dg_vec
                                  );
            

            
            //            sMI_i_j = max2
            //            (
            //             sMM[j] + q->tr[i-1][M2M] + t->tr[j][M2I], // MM->MI gap opening M2I in template
            //             sMI[j] + q->tr[i-1][M2M] + t->tr[j][I2I], // MI->MI gap extension I2I in template
            //             bMI[i][j]
            //             );
            simd_float mm_mi_vec = simdf32_add( simdf32_add(sMM_j, q_m2m), t_m2i);  // MM->MI gap opening M2I in template
            simd_float mi_mi_vec = simdf32_add( simdf32_add(sMI_j, q_m2m), t_i2i);  // MI->MI gap extension I2I in template
            // if mm_mi > mi_mi { 64 }
            MAX2_SET_MASK(mm_mi_vec, mi_mi_vec,mi_mm_vec, byte_result_vec);
            
            sMI_i_j = simdf32_max(
                                  mm_mi_vec,
                                  mi_mi_vec
                                  );

            
            // Cell of logic
            // if (cell_off[i][j])
            //shift   10000000100000001000000010000000 -> 01000000010000000100000001000000
            //because 10000000000000000000000000000000 = -2147483648 kills cmplt
#ifdef VITERBI_CELLOFF
#ifdef AVX2_SUPPORT
//            if(((sCO_MI_DG_IM_GD_MM_vec[j]  >>1) & 0x4040404040404040) > 0){
//                std::cout << ((sCO_MI_DG_IM_GD_MM_vec[j]  >>1) & 0x4040404040404040   ) << std::endl;
//            }
            simd_int matrix_vec    = _mm256_set1_epi64x(sCO_MI_DG_IM_GD_MM_vec[j]>>1);
            matrix_vec             = _mm256_shuffle_epi8(matrix_vec,shuffle_mask_celloff);
#else
//            if(((sCO_MI_DG_IM_GD_MM_vec[j]  >>1) & 0x40404040) > 0){
//                std::cout << ((sCO_MI_DG_IM_GD_MM_vec[j]  >>1) & 0x40404040   ) << std::endl;
//            }
            simd_int matrix_vec    = simdi32_set(sCO_MI_DG_IM_GD_MM_vec[j]>>1);

#endif
            simd_int cell_off_vec  = simdi_and(matrix_vec, co_vec);
            simd_int res_eq_co_vec = simdi32_gt(co_vec, cell_off_vec    ); // shift is because signed can't be checked here
            simd_float  cell_off_float_min_vec = (simd_float) simdi_andnot(res_eq_co_vec, float_min_vec); // inverse

//            if(((sCO_MI_DG_IM_GD_MM_vec[j]  >>1) & 0x4040404040404040) > 0){
//                for(int i = 0; i < 8; i++){
//                    std::cout << ((float *) &cell_off_float_min_vec )[i] << " ";
//                }
//                std::cout << std::endl;
//            }
            sMM_i_j = simdf32_add(sMM_i_j,cell_off_float_min_vec);    // add the cell off vec to sMM_i_j. Set -FLT_MAX to cell off
            sGD_i_j = simdf32_add(sGD_i_j,cell_off_float_min_vec);
            sIM_i_j = simdf32_add(sIM_i_j,cell_off_float_min_vec);
            sDG_i_j = simdf32_add(sDG_i_j,cell_off_float_min_vec);
            sMI_i_j = simdf32_add(sMI_i_j,cell_off_float_min_vec);
#endif
            
            
            
            simdf32_store((float *)(sMM_DG_MI_GD_IM_vec+index_pos_j + 0), sMM_i_j);
            simdf32_store((float *)(sMM_DG_MI_GD_IM_vec+index_pos_j + 1), sDG_i_j);
            simdf32_store((float *)(sMM_DG_MI_GD_IM_vec+index_pos_j + 2), sMI_i_j);
            simdf32_store((float *)(sMM_DG_MI_GD_IM_vec+index_pos_j + 3), sGD_i_j);
            simdf32_store((float *)(sMM_DG_MI_GD_IM_vec+index_pos_j + 4), sIM_i_j);

            // write values back to ViterbiMatrix
#ifdef AVX2_SUPPORT
            /* byte_result_vec        000H  000G  000F  000E   000D  000C  000B  000A */
            /* abcdefgh               0000  0000  HGFE  0000   0000  0000  0000  DCBA */
            const __m256i abcdefgh = _mm256_shuffle_epi8(byte_result_vec, shuffle_mask_extract);
            /* abcd                                            0000  0000  0000  DCBA */
            const __m128i abcd     = _mm256_castsi256_si128(abcdefgh);
            /* efgh                                            0000  0000  HGFE  0000 */
            const __m128i efgh     = _mm256_extracti128_si256(abcdefgh, 1);
            _mm_storel_epi64((__m128i*)&sCO_MI_DG_IM_GD_MM_vec[j], _mm_or_si128(abcd, efgh));
#else
            byte_result_vec = _mm_packs_epi32(byte_result_vec, byte_result_vec);
            byte_result_vec = _mm_packus_epi16(byte_result_vec, byte_result_vec);
            int int_result  = _mm_cvtsi128_si32(byte_result_vec);
            sCO_MI_DG_IM_GD_MM_vec[j] = int_result;
#endif
            

            
            // Find maximum score; global alignment: maxize only over last row and last column
            // if(sMM_i_j>score && (par.loc || i==q->L)) { i2=i; j2=j; score=sMM_i_j; }
            if (findMaxInnerLoop){
                
                // new score is higer
                // output
                //  0   0   0   MAX
                simd_int lookup_mask_hi = (simd_int) simdf32_gt(sMM_i_j,score_vec);
                // old score is higher
                // output
                //  MAX MAX MAX 0
                simd_int lookup_mask_lo = (simd_int) simdf32_lt(sMM_i_j,score_vec);
                
                
                simd_int curr_pos_j   = simdi32_set(j);
                simd_int new_j_pos_hi = simdi_and(lookup_mask_hi,curr_pos_j);
                simd_int old_j_pos_lo = simdi_and(lookup_mask_lo,j2_vec);
                j2_vec = simdi32_add(new_j_pos_hi,old_j_pos_lo);
                simd_int curr_pos_i   = simdi32_set(i);
                simd_int new_i_pos_hi = simdi_and(lookup_mask_hi,curr_pos_i);
                simd_int old_i_pos_lo = simdi_and(lookup_mask_lo,i2_vec);
                i2_vec = simdi32_add(new_i_pos_hi,old_i_pos_lo);
                
                score_vec=simdf32_max(sMM_i_j,score_vec);
            }
            
            
            
        } //end for j
        
        // if global alignment: look for best cell in last column
        if (!local){
            
            // new score is higer
            // output
            //  0   0   0   MAX
            simd_int lookup_mask_hi = (simd_int) simdf32_gt(sMM_i_j,score_vec);
            // old score is higher
            // output
            //  MAX MAX MAX 0
            simd_int lookup_mask_lo = (simd_int) simdf32_lt(sMM_i_j,score_vec);

            
            simd_int curr_pos_j   = simdi32_set(j);
            simd_int new_j_pos_hi = simdi_and(lookup_mask_hi,curr_pos_j);
            simd_int old_j_pos_lo = simdi_and(lookup_mask_lo,j2_vec);
            j2_vec = simdi32_add(new_j_pos_hi,old_j_pos_lo);
            simd_int curr_pos_i   = simdi32_set(i);
            simd_int new_i_pos_hi = simdi_and(lookup_mask_hi,curr_pos_i);
            simd_int old_i_pos_lo = simdi_and(lookup_mask_lo,i2_vec);
            i2_vec = simdi32_add(new_i_pos_hi,old_i_pos_lo);
            
            score_vec = simdf32_max(sMM_i_j,score_vec);
        }    // end for j
    }     // end for i
    
    for(int seq_index=0; seq_index < maxres; seq_index++){
        result->score[seq_index]=((float*)&score_vec)[seq_index];
        result->i[seq_index] = ((int*)&i2_vec)[seq_index];
        result->j[seq_index] = ((int*)&j2_vec)[seq_index];
    }
    
    //   printf("Template=%-12.12s  i=%-4i j=%-4i score=%6.3f\n",t->name,i2,j2,score);
}


#undef MAX2_SET_MASK
#undef MAX2


